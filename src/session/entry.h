#pragma once

#include "base/array.h"
#include "base/str.h"
#include "llm/types.h"
#include "session/id.h"
#include "session/result.h"

// v2 is an intentionally incompatible, Pi-style message-oriented schema: it
// persists only the session tree and its model-visible state (messages,
// model/thinking/tool settings, compactions, branch labels/names, and the
// active leaf). Crash-recovery journal records (operations, generations,
// tool-execution intents, queues, deferred writes, cancellation, recovery
// resolutions) are gone; the harness keeps active turns and queues in memory
// only, matching Pi's simpler persistence and crash semantics.
constexpr u32 SESSION_SCHEMA_VERSION = 2;

enum class SessionRecordKind : u8 {
  SessionStarted,
  Message,
  Model,
  Thinking,
  Tools,
  Compaction,
  BranchLabel,
  SessionName,
  Leaf,
};

struct SessionStartedData {
  StrView WorkingDirectory;
  StrView ProjectKey;
  StrView Label;
  DurableId ForkedFromSessionId;
  DurableId ForkedFromEntryId;
};

struct MessageEntryData {
  DurableId ParentId;
  Message Value;
};

struct ModelEntryData {
  DurableId ParentId;
  StrView Provider;
  StrView Model;
};

struct ThinkingEntryData {
  DurableId ParentId;
  StrView Level;
};

struct ToolsEntryData {
  DurableId ParentId;
  Array<StrView> ToolNames;
};

struct CompactionEntryData {
  DurableId ParentId;
  DurableId BaseEntryId;
  DurableId ThroughEntryId;
  DurableId SourceHeadId;
  Message Summary;
  bool HasUsage;
  Usage Use;
  // Files the folded span touched, so the paths survive the turns being
  // summarized away. `ReadFiles` holds paths that were only read; a path that
  // was written or edited appears in `ModifiedFiles` instead. Both are sorted
  // and deduplicated, and each compaction reseeds from the ones before it,
  // which is what makes the tracking cumulative. Absent in logs written before
  // the fields existed, which decodes as empty.
  Array<StrView> ReadFiles;
  Array<StrView> ModifiedFiles;
};

struct BranchLabelEntryData {
  DurableId ParentId;
  StrView Label;
};

struct SessionNameEntryData {
  StrView Name;
};

// Explicit marker for the active leaf entry. Appended whenever the harness
// moves the head to an entry other than the one the default parent-chaining
// rule would select (checkout/navigate to an existing entry). Ordinary
// appends (message/model/thinking/tools/branch-label/compaction) become the
// new head implicitly because their parentId equals the current head.
struct LeafEntryData {
  DurableId LeafId;
};

struct SessionRecord {
  u32 SchemaVersion;
  DurableId SessionId;
  u64 Seq;
  DurableId EntryId;
  SessionRecordKind Kind;
  u64 CreatedAt;

  union {
    SessionStartedData SessionStarted;
    MessageEntryData MessageEntry;
    ModelEntryData ModelEntry;
    ThinkingEntryData ThinkingEntry;
    ToolsEntryData ToolsEntry;
    CompactionEntryData CompactionEntry;
    BranchLabelEntryData BranchLabelEntry;
    SessionNameEntryData SessionNameEntry;
    LeafEntryData LeafEntry;
  };
};

// True for record kinds that occupy a position in the entry tree and can
// therefore be validly referenced by EntryId (as a parent, a compaction
// base/through/source-head, or a leaf target): everything except the
// session-name label and the leaf marker itself, which are never targets of
// such a reference.
bool SessionRecord_IsTreeNode(SessionRecordKind kind);
StrView SessionRecordKind_Name(SessionRecordKind kind);
// Returns the zero DurableId for kinds without a parent (SessionStarted,
// SessionName, Leaf).
DurableId SessionRecord_ParentId(const SessionRecord* record);
SessionResult SessionRecord_PrepareForkCopy(
    const SessionRecord* source, DurableId targetSessionId, u64 targetSeq,
    u64 createdAt, const Array<DurableId>* oldIds,
    const Array<DurableId>* newIds, SessionRecord* out);

// Encodes exactly one JSON object without a trailing newline.
SessionResult SessionRecord_Encode(StrBuf* out, const SessionRecord* record);

// Input must be null-terminated at text.Data[text.Length], as required by the
// JSON parser. All decoded strings and arrays are owned by arena.
SessionResult SessionRecord_Decode(Arena* arena, StrView text, SessionRecord* out);

// Explicit deep structural clone: every string, content array, and nested
// array is copied field-by-field into `arena`. Never round-trips through the
// JSON codec (that would cost an encode+parse per historical record).
SessionResult SessionRecord_Clone(Arena* arena, const SessionRecord* source,
                                  SessionRecord* out);

// Deep clone of one message: content array, text, and every string are copied
// into `arena`, so the result shares nothing with `source`.
Message Message_Clone(Arena* arena, const Message& source);

// Validates envelope fields and kind-specific required values for a single
// record in isolation (no history/index required).
SessionResult SessionRecord_Validate(const SessionRecord* record);
