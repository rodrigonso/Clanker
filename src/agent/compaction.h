#pragma once

#include "llm/types.h"
#include "session/entry.h"

// Automatic compaction policy. Mirrors pi's `compaction` settings block:
// compaction fires once a turn's context has consumed the model's window down
// to `ReserveTokens` of headroom, and keeps roughly `KeepRecentTokens` worth of
// the most recent messages verbatim instead of folding the whole branch away.
struct AgentCompactionPolicy {
  bool Enabled;
  u32 ReserveTokens;
  u32 KeepRecentTokens;
};

constexpr u32 AGENT_COMPACTION_DEFAULT_RESERVE_TOKENS = 16384;
constexpr u32 AGENT_COMPACTION_DEFAULT_KEEP_RECENT_TOKENS = 20000;

// Bytes per token used only when no provider usage is available. Deliberately
// coarse: `ReserveTokens` is what absorbs the error.
constexpr u32 AGENT_COMPACTION_BYTES_PER_TOKEN = 4;

inline AgentCompactionPolicy AgentCompactionPolicy_Default() {
  AgentCompactionPolicy policy = {};
  policy.Enabled = true;
  policy.ReserveTokens = AGENT_COMPACTION_DEFAULT_RESERVE_TOKENS;
  policy.KeepRecentTokens = AGENT_COMPACTION_DEFAULT_KEEP_RECENT_TOKENS;
  return policy;
}

// A policy is only actionable when it is enabled and keeps a smaller tail than
// the headroom it defends; otherwise compaction could never lower pressure.
bool AgentCompactionPolicy_IsValid(const AgentCompactionPolicy* policy);

// Byte-derived token estimate for one message and for a materialized context.
u32 AgentCompaction_EstimateMessageTokens(const Message* message);
u32 AgentCompaction_EstimateContextTokens(const Message* messages, u32 count);

// Prompt tokens the provider actually billed, carried forward from the most
// recent assistant message that reported usage: its whole prompt
// (input + cache read + cache write) plus its own output, plus a byte estimate
// for everything appended after it. Returns 0 when no usage has been reported.
u32 AgentCompaction_ReportedContextTokens(const Message* messages, u32 count);

// Provider-reported usage when available, byte estimate otherwise.
u32 AgentCompaction_ContextTokens(const Message* messages, u32 count);

// True once `contextTokens` has eaten into the window's reserved headroom. A
// zero `contextWindow` means the model published no window, so nothing fires.
bool AgentCompaction_ShouldCompact(const AgentCompactionPolicy* policy,
                                   u32 contextWindow, u32 contextTokens);

// The span of branch entries a compaction folds into its summary. `Base` and
// `Through` are inclusive and always name tree nodes on the branch.
struct AgentCompactionCut {
  DurableId BaseEntryId;
  DurableId ThroughEntryId;
  bool HasRetainedTail;
};

// Chooses the span to compact so that roughly `keepRecentTokens` of the newest
// messages survive verbatim.
//
// The retained tail may only begin at a `Role::User` message. That is what
// guarantees an assistant tool-call message is never separated from its
// `Role::ToolResult` messages, since tool results always follow their assistant
// message with no intervening user turn.
//
// Returns false when there is nothing worth compacting (no messages, or the
// whole branch would be retained). Falls back to spanning the entire branch
// when no user boundary fits the budget; pi's two-phase split-turn summary is
// not implemented.
bool AgentCompaction_SelectCutPoint(const SessionRecord* const* branch, u32 branchCount,
                                    u32 keepRecentTokens, AgentCompactionCut* out);

// Files the agent touched, split by how it touched them. Mirrors pi's
// `FileOperations`: paths are the `path` argument of `read`, `write`, and
// `edit` tool calls. Commands run through `shell` are not inspected, so files
// they create are not tracked -- the same blind spot pi has.
struct AgentFileOps {
  Array<StrView> Read;
  Array<StrView> Written;
  Array<StrView> Edited;
};

// The two lists a compaction reports: paths that were only read, and paths that
// were written or edited. Sorted and deduplicated.
struct AgentFileLists {
  Array<StrView> ReadFiles;
  Array<StrView> ModifiedFiles;
};

// Unions every tracked tool-call path on `branch`, up to and including
// `throughEntryId`, with the lists already stored on any Compaction record in
// that span. Reading those stored lists is what carries paths across successive
// compactions, since the turns they came from have been folded away. Paths are
// copied into `arena`. A zero `throughEntryId` scans the whole branch.
void AgentCompaction_CollectFileOps(Arena* arena, const SessionRecord* const* branch,
                                    u32 branchCount, DurableId throughEntryId,
                                    AgentFileOps* out);

// A path that was written or edited is reported only as modified, never as read.
void AgentCompaction_BuildFileLists(Arena* arena, const AgentFileOps* ops,
                                    AgentFileLists* out);

// Appends pi's `<read-files>` / `<modified-files>` sections to a summary body.
// Appends nothing when both lists are empty.
void AgentCompaction_AppendFileSections(StrBuf* out, const AgentFileLists* lists);
