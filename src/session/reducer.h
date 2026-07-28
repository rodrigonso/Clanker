#pragma once

#include "session/entry.h"

enum class SessionSuspensionKind : u8 {
  None,
  Operation,
  Generation,
  ReplaySafeTool,
  ManualTool,
};

struct SessionNode {
  DurableId EntryId;
  DurableId ParentId;
  u32 RecordIndex;
};

struct SessionReduction {
  const SessionRecord* Records;
  u32 RecordCount;
  Array<SessionNode> Nodes;
  Array<const SessionRecord*> Branch;
  DurableId HeadId;

  StrView Provider;
  StrView Model;
  StrView ThinkingLevel;
  Array<StrView> ToolNames;

  bool HasActiveOperation;
  const SessionRecord* ActiveOperationStart;
  DurableId ActiveOperationId;
  OperationKind ActiveOperationKind;
  bool CancelRequested;
  SessionSuspensionKind Suspension;
  DurableId PendingAttemptId;
  const SessionRecord* PendingGeneration;
  Array<const SessionRecord*> PendingTools;
  Array<const SessionRecord*> PendingQueueItems;
  Array<const SessionRecord*> PendingDeferredWrites;
};

SessionResult SessionReducer_Reduce(Arena* arena, const SessionRecord* records, u32 count,
                                    SessionReduction* out);

SessionResult SessionReducer_Ancestry(Arena* arena, const SessionReduction* reduction,
                                      DurableId headId,
                                      Array<const SessionRecord*>* outBranch);

// Materializes the model-visible message sequence for a branch. Harness
// records and branch settings never enter this array.
SessionResult SessionReducer_Context(Arena* arena, const SessionReduction* reduction,
                                     DurableId headId, Array<Message>* outMessages);
