#pragma once
#include "base/str.h"

struct SearchRegexState;

// All engine storage belongs to the supplied arena. Reuse one instance serially
// across lines; releasing the arena is the only cleanup required.
struct SearchRegex {
  SearchRegexState* State;
};

bool SearchRegex_Compile(Arena* arena, StrView pattern, bool literal,
                         bool ignoreCase, volatile i32* abort,
                         SearchRegex* out, StrBuf* err);
bool SearchRegex_Match(SearchRegex* regex, StrView line, bool* matched,
                       StrBuf* err);
