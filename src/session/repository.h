#pragma once

#include "base/array.h"
#include "session/metadata.h"

enum class SessionListScope : u8 { CurrentProject, AllProjects };

struct SessionListOptions {
  SessionListScope Scope;
  StrView ProjectKey;
  u32* SkippedCount;
};

struct SessionCreateOptions {
  DurableId RequestedId;
  StrView ProjectKey;
  StrView WorkingDirectory;
  StrView Label;
  DurableId ForkedFromSessionId;
  DurableId ForkedFromEntryId;
};

struct SessionOpenOptions {
  DurableId SessionId;
  StrView ProjectKey;
  bool Writable;
};

struct SessionForkOptions {
  DurableId SourceSessionId;
  DurableId SourceHeadId;
  DurableId RequestedTargetId;
  StrView ProjectKey;
  StrView WorkingDirectory;
  StrView Label;
};

struct SessionLog {
  void* Ctx;
  bool Writable;
  SessionResult (*ReadAll)(void* ctx, Arena* arena, Array<SessionRecord>* out);
  SessionResult (*Append)(void* ctx, SessionRecord* record);
  SessionResult (*AppendBatch)(void* ctx, SessionRecord* records, u32 count);
  void (*Close)(void* ctx);
};

inline SessionResult SessionLog_ReadAll(SessionLog* log, Arena* arena,
                                        Array<SessionRecord>* out) {
  if (!log || !log->ReadAll)
    return SessionResult_Error(SessionStatus::InvalidState, SVL("session log is closed"));
  return log->ReadAll(log->Ctx, arena, out);
}

inline SessionResult SessionLog_Append(SessionLog* log, SessionRecord* record) {
  if (!log || !log->Append || !log->Writable)
    return SessionResult_Error(SessionStatus::InvalidState,
                               SVL("session log is not writable"));
  return log->Append(log->Ctx, record);
}

inline SessionResult SessionLog_AppendBatch(SessionLog* log, SessionRecord* records,
                                            u32 count) {
  if (!log || !log->AppendBatch || !log->Writable)
    return SessionResult_Error(SessionStatus::InvalidState,
                               SVL("session log is not writable"));
  return log->AppendBatch(log->Ctx, records, count);
}

inline void SessionLog_Close(SessionLog* log) {
  if (!log) return;
  if (log->Close) log->Close(log->Ctx);
  *log = SessionLog{};
}

struct SessionRepository {
  void* Ctx;
  SessionResult (*Create)(void* ctx, Arena* arena, const SessionCreateOptions* options,
                          SessionLog* outLog, SessionInfo* outInfo);
  SessionResult (*Open)(void* ctx, Arena* arena, const SessionOpenOptions* options,
                        SessionLog* outLog, SessionInfo* outInfo);
  SessionResult (*Resolve)(void* ctx, Arena* arena, DurableId sessionId,
                           SessionInfo* outInfo);
  SessionResult (*List)(void* ctx, Arena* arena,
                        const SessionListOptions* options,
                        Array<SessionInfo>* outItems);
  SessionResult (*Fork)(void* ctx, Arena* arena, const SessionForkOptions* options,
                        SessionLog* outLog, SessionInfo* outInfo);
  void (*Close)(void* ctx);
};
