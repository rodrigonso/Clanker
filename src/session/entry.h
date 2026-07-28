#pragma once

#include "base/array.h"
#include "base/str.h"
#include "llm/types.h"
#include "session/id.h"
#include "session/replay.h"
#include "session/result.h"

constexpr u32 SESSION_SCHEMA_VERSION = 1;

enum class SessionEntryClass : u8 { Session, Harness };

enum class SessionRecordKind : u8 {
  SessionStarted,
  Message,
  Model,
  Thinking,
  Tools,
  Compaction,
  BranchLabel,
  OperationStarted,
  GenerationStarted,
  GenerationFinished,
  ToolStarted,
  ToolFinished,
  QueueItemAccepted,
  QueueCheckpoint,
  DeferredWrite,
  DeferredWriteCommitted,
  OperationCancelRequested,
  OperationFinished,
  OperationAborted,
  RecoveryResolved,
};

enum class OperationKind : u8 { Run, Compaction, Navigation };
enum class GenerationOutcome : u8 { Succeeded, Failed, Aborted };
enum class ToolOutcomeKind : u8 { Succeeded, Failed, Aborted };
enum class OperationOutcome : u8 { Succeeded, Failed, Cancelled };
enum class QueueKind : u8 { Steering, FollowUp, NextTurn };
enum class QueueCheckpointKind : u8 { BeforeAssistant, AfterToolBatch, AfterRun };
enum class RecoveryResolution : u8 { RecordFailure, SupplyResult, RerunAnyway };

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
};

struct BranchLabelEntryData {
  DurableId ParentId;
  StrView Label;
};

struct OperationStartedData {
  DurableId OperationId;
  OperationKind Operation;
  DurableId StartingHeadId;
  bool HasAcceptedMessage;
  Message AcceptedMessage;
  DurableId ProvisionedEntryId;
  DurableId TargetEntryId;
};

struct GenerationStartedData {
  DurableId OperationId;
  DurableId AttemptId;
  StrView Provider;
  StrView Model;
  StrView EffectiveRequestJson;
  i32 MaxTokens;
  DurableId ProvisionedAssistantEntryId;
};

struct GenerationFinishedData {
  DurableId OperationId;
  DurableId AttemptId;
  GenerationOutcome Outcome;
  DurableId MessageEntryId;
  StrView Error;
};

struct ToolStartedData {
  DurableId OperationId;
  DurableId ToolExecutionId;
  StrView ToolCallId;
  StrView ToolName;
  StrView EffectiveArgumentsJson;
  DurableId ProvisionedResultEntryId;
  ToolReplayPolicy ReplayPolicy;
};

struct ToolFinishedData {
  DurableId OperationId;
  DurableId ToolExecutionId;
  ToolOutcomeKind Outcome;
  DurableId ResultEntryId;
  bool Terminate;
  StrView Error;
};

struct QueueItemAcceptedData {
  DurableId OperationId;
  DurableId ItemId;
  QueueKind Queue;
  Message Value;
};

struct QueueCheckpointData {
  DurableId OperationId;
  QueueCheckpointKind Checkpoint;
  Array<DurableId> ItemIds;
};

struct DeferredWriteData {
  DurableId OperationId;
  DurableId DeferredId;
  DurableId QueueItemId;
  DurableId ProvisionedEntryId;
  Message Value;
};

struct DeferredWriteCommittedData {
  DurableId OperationId;
  DurableId DeferredId;
  DurableId MessageEntryId;
};

struct OperationCancelRequestedData {
  DurableId OperationId;
};

struct OperationFinishedData {
  DurableId OperationId;
  OperationOutcome Outcome;
  DurableId ResultHeadId;
  StrView Error;
};

struct OperationAbortedData {
  DurableId OperationId;
  StrView Reason;
};

struct RecoveryResolvedData {
  DurableId OperationId;
  DurableId ToolExecutionId;
  RecoveryResolution Resolution;
  bool HasSuppliedResult;
  Message SuppliedResult;
};

struct SessionRecord {
  u32 SchemaVersion;
  DurableId SessionId;
  u64 Seq;
  DurableId EntryId;
  SessionEntryClass EntryClass;
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
    OperationStartedData OperationStarted;
    GenerationStartedData GenerationStarted;
    GenerationFinishedData GenerationFinished;
    ToolStartedData ToolStarted;
    ToolFinishedData ToolFinished;
    QueueItemAcceptedData QueueItemAccepted;
    QueueCheckpointData QueueCheckpoint;
    DeferredWriteData DeferredWrite;
    DeferredWriteCommittedData DeferredWriteCommitted;
    OperationCancelRequestedData OperationCancelRequested;
    OperationFinishedData OperationFinished;
    OperationAbortedData OperationAborted;
    RecoveryResolvedData RecoveryResolved;
  };
};

SessionEntryClass SessionRecord_ClassForKind(SessionRecordKind kind);
StrView SessionRecordKind_Name(SessionRecordKind kind);

// Encodes exactly one JSON object without a trailing newline.
SessionResult SessionRecord_Encode(StrBuf* out, const SessionRecord* record);

// Input must be null-terminated at text.Data[text.Length], as required by the
// JSON parser. All decoded strings and arrays are owned by arena.
SessionResult SessionRecord_Decode(Arena* arena, StrView text, SessionRecord* out);

// Deep-copies all arena-backed strings and arrays into arena by round-tripping
// through the canonical codec.
SessionResult SessionRecord_Clone(Arena* arena, const SessionRecord* source,
                                  SessionRecord* out);

// Validates envelope fields, kind-specific required values, total sequence
// order, tree references, operation transitions, and provisioned result links.
SessionResult SessionRecord_Validate(const SessionRecord* record);
SessionResult SessionRecords_Validate(const SessionRecord* records, u32 count);
