#include "tools/search_regex.h"
#include "base/thread.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

static const usize RegexMemoryLimit = (usize)32 * 1024 * 1024;
static const u32 RegexPatternLimit = 64 * 1024;
static const u32 RegexLineLimit = 16 * 1024 * 1024;
static const u32 RegexCalloutLimit = 10000000;
static const u64 RegexTimeLimitMs = 1000;

struct SearchRegexState {
  Arena* Backing;
  volatile i32* Abort;
  pcre2_code* Code;
  pcre2_match_data* MatchData;
  pcre2_match_context* MatchContext;
  usize Allocated;
  u32 Callouts;
  u64 StartedMs;
};

static bool RegexError(StrBuf* err, const char* message) {
  StrBuf_AppendCStr(err, message);
  return false;
}

static void RegexEngineError(StrBuf* err, int code) {
  PCRE2_UCHAR message[256];
  int length = pcre2_get_error_message(code, message, sizeof(message));
  if (length >= 0) {
    StrBuf_Append(err, StrView{(const char*)message, (u32)length});
  } else {
    StrBuf_AppendCStr(err, "PCRE2 error ");
    StrBuf_AppendI64(err, code);
  }
}

static void* RegexAlloc(usize size, void* user) {
  SearchRegexState* state = (SearchRegexState*)user;
  Arena* arena = state->Backing;
  const usize alignment =
      alignof(void*) > alignof(long double) ? alignof(void*) : alignof(long double);
  const usize mask = alignment - 1;
  usize padding = (0 - arena->Used) & mask;
  if (padding > RegexMemoryLimit - state->Allocated ||
      size > RegexMemoryLimit - state->Allocated - padding ||
      padding > arena->Reserved - arena->Used ||
      size > arena->Reserved - arena->Used - padding)
    return nullptr;
  state->Allocated += size + padding;
  return ArenaAlloc(arena, size, alignment);
}

static void RegexFree(void*, void*) {
  // PCRE2 retains its geometrically grown backtracking buffer in MatchData.
  // No per-line allocation occurs after that buffer reaches its high-water mark;
  // abandoned growth buffers and compile workspace count against the same cap.
}

static int RegexCompileGuard(u32, void* user) {
  SearchRegexState* state = (SearchRegexState*)user;
  return state->Abort && Atomic_LoadI32(state->Abort) ? 1 : 0;
}

static int RegexCallout(pcre2_callout_block*, void* user) {
  SearchRegexState* state = (SearchRegexState*)user;
  if (state->Abort && Atomic_LoadI32(state->Abort))
    return PCRE2_ERROR_CALLOUT;
  // PCRE2's match limit restarts at each candidate start position. This budget
  // spans the entire line, including all unanchored attempts.
  if (++state->Callouts > RegexCalloutLimit)
    return PCRE2_ERROR_MATCHLIMIT;
  if ((state->Callouts & 255) == 0 &&
      MonotonicMillis() - state->StartedMs >= RegexTimeLimitMs)
    return PCRE2_ERROR_MATCHLIMIT;
  return 0;
}

static bool RegexMetachar(char c) {
  switch (c) {
    case '\\': case '.': case '^': case '$': case '|': case '?': case '*':
    case '+': case '(': case ')': case '[': case ']': case '{': case '}':
      return true;
    default:
      return false;
  }
}

bool SearchRegex_Compile(Arena* arena, StrView pattern, bool literal,
                         bool ignoreCase, volatile i32* abort,
                         SearchRegex* out, StrBuf* err) {
  *out = {};
  if (abort && Atomic_LoadI32(abort))
    return RegexError(err, "regex parse error: search aborted");
  if (pattern.Length > RegexPatternLimit)
    return RegexError(err, "regex parse error: pattern exceeds 64 KiB limit");
  if (pattern.Length && !pattern.Data)
    return RegexError(err, "regex parse error: null pattern data");

  SearchRegexState allocator = {};
  allocator.Backing = arena;
  SearchRegexState* state =
      (SearchRegexState*)RegexAlloc(sizeof(SearchRegexState), &allocator);
  if (!state)
    return RegexError(err, "regex parse error: memory limit exceeded");
  *state = allocator;
  state->Abort = abort;

  // Escape instead of PCRE2_LITERAL: literal mode cannot be combined with the
  // auto-callouts that enforce cancellation and the whole-line work budget.
  if (literal) {
    u32 escapes = 0;
    for (u32 i = 0; i < pattern.Length; i++)
      if (RegexMetachar(pattern.Data[i])) escapes++;
    if (escapes) {
      char* escaped = (char*)RegexAlloc(pattern.Length + escapes, state);
      if (!escaped)
        return RegexError(err, "regex parse error: memory limit exceeded");
      u32 length = 0;
      for (u32 i = 0; i < pattern.Length; i++) {
        if (RegexMetachar(pattern.Data[i])) escaped[length++] = '\\';
        escaped[length++] = pattern.Data[i];
      }
      pattern = StrView{escaped, length};
    }
  }

  pcre2_general_context* general =
      pcre2_general_context_create(RegexAlloc, RegexFree, state);
  if (!general)
    return RegexError(err, "regex parse error: memory limit exceeded");
  pcre2_compile_context* compile = pcre2_compile_context_create(general);
  if (!compile)
    return RegexError(err, "regex parse error: memory limit exceeded");

  if (pcre2_set_parens_nest_limit(compile, 250) != 0 ||
      pcre2_set_max_pattern_length(compile, RegexPatternLimit * 2) != 0 ||
      pcre2_set_max_pattern_compiled_length(compile, 1024 * 1024) != 0 ||
      pcre2_set_newline(compile, PCRE2_NEWLINE_LF) != 0 ||
      pcre2_set_compile_extra_options(compile, PCRE2_EXTRA_NEVER_CALLOUT) != 0 ||
      pcre2_set_compile_recursion_guard(compile, RegexCompileGuard, state) != 0)
    return RegexError(err, "regex parse error: unsupported PCRE2 configuration");

  u32 options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_INVALID_UTF |
                PCRE2_AUTO_CALLOUT | PCRE2_NEVER_BACKSLASH_C;
  if (ignoreCase) options |= PCRE2_CASELESS;
  int errorCode = 0;
  PCRE2_SIZE errorOffset = 0;
  state->Code = pcre2_compile((PCRE2_SPTR)(pattern.Data ? pattern.Data : ""),
                              pattern.Length, options, &errorCode, &errorOffset,
                              compile);
  if (abort && Atomic_LoadI32(abort))
    return RegexError(err, "regex parse error: search aborted");
  if (!state->Code) {
    StrBuf_AppendCStr(err, "regex parse error at byte ");
    StrBuf_AppendU64(err, errorOffset);
    StrBuf_AppendCStr(err, ": ");
    RegexEngineError(err, errorCode);
    return false;
  }

  // Only the overall match matters; captures stay in PCRE2's bounded frames.
  state->MatchData = pcre2_match_data_create(1, general);
  state->MatchContext = pcre2_match_context_create(general);
  if (!state->MatchData || !state->MatchContext)
    return RegexError(err, "regex parse error: memory limit exceeded");
  if (pcre2_set_heap_limit(state->MatchContext, 8 * 1024) != 0 ||
      pcre2_set_depth_limit(state->MatchContext, 10000) != 0 ||
      pcre2_set_match_limit(state->MatchContext, 1000000) != 0 ||
      pcre2_set_callout(state->MatchContext, RegexCallout, state) != 0)
    return RegexError(err, "regex parse error: unsupported PCRE2 configuration");

  out->State = state;
  return true;
}

bool SearchRegex_Match(SearchRegex* regex, StrView line, bool* matched,
                       StrBuf* err) {
  *matched = false;
  SearchRegexState* state = regex->State;
  if (!state)
    return RegexError(err, "regex match error: pattern is not compiled");
  if (state->Abort && Atomic_LoadI32(state->Abort))
    return RegexError(err, "regex match error: search aborted");
  if (line.Length > RegexLineLimit)
    return RegexError(err, "regex match error: line exceeds 16 MiB limit");
  if (line.Length && !line.Data)
    return RegexError(err, "regex match error: null line data");

  state->Callouts = 0;
  state->StartedMs = MonotonicMillis();
  int result = pcre2_match(state->Code,
                           (PCRE2_SPTR)(line.Data ? line.Data : ""), line.Length,
                           0, 0, state->MatchData, state->MatchContext);
  if (state->Abort && Atomic_LoadI32(state->Abort))
    return RegexError(err, "regex match error: search aborted");
  if (result == PCRE2_ERROR_MATCHLIMIT ||
      MonotonicMillis() - state->StartedMs >= RegexTimeLimitMs)
    return RegexError(err, "regex match error: work limit exceeded");
  if (result >= 0) {
    *matched = true;
    return true;
  }
  if (result == PCRE2_ERROR_NOMATCH) return true;
  StrBuf_AppendCStr(err, "regex match error: ");
  RegexEngineError(err, result);
  return false;
}
