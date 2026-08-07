#include "session/memory_repository.h"

#include "session/reducer.h"

struct MemoryLogHandle;

struct MemorySession {
  Array<SessionRecord> Records;
  // Persistent fold state: built once (CreateSessionLocked) and updated in
  // O(1) per appended record (SessionReducer_Fold). Never rebuilt via
  // SessionReducer_Reduce on a hot append/open path.
  SessionReduction Reduction;
  MemoryLogHandle* Writer;
};

struct MemoryLogHandle {
  MemoryRepository* Repository;
  MemorySession* Session;
  bool Writable;
  bool Faulted;
  bool Closed;
};

namespace {

const SessionRecord* SessionRoot(const MemorySession* session) {
  return session && session->Records.Count ? &session->Records.Data[0]
                                           : nullptr;
}

DurableId SessionId(const MemorySession* session) {
  const SessionRecord* root = SessionRoot(session);
  return root ? root->SessionId : DurableId{};
}

StrView ProjectKey(const MemorySession* session) {
  const SessionRecord* root = SessionRoot(session);
  return root ? root->SessionStarted.ProjectKey : StrView{};
}

u64 LastSeq(const MemorySession* session) {
  return session && session->Records.Count
             ? session->Records.Data[session->Records.Count - 1].Seq
             : 0;
}

MemorySession* FindSession(MemoryRepository* repository, StrView projectKey,
                           DurableId sessionId) {
  for (u32 i = 0; i < repository->Sessions.Count; i++) {
    MemorySession* session = repository->Sessions.Data[i];
    if (DurableId_Eq(SessionId(session), sessionId) &&
        SessionProjectKey_Eq(ProjectKey(session), projectKey))
      return session;
  }
  return nullptr;
}

bool IdExists(MemoryRepository* repository, DurableId sessionId) {
  for (u32 i = 0; i < repository->Sessions.Count; i++) {
    if (DurableId_Eq(SessionId(repository->Sessions.Data[i]), sessionId))
      return true;
  }
  return false;
}

SessionResult BuildInfo(Arena* arena, const MemorySession* session,
                        SessionInfo* out) {
  SessionMetadataBuildOptions options = {};
  options.Busy = session->Writer != nullptr;
  options.NowMs = NowMillis();
  return SessionMetadata_Build(arena, session->Records.Data,
                               session->Records.Count, &options, out);
}

MemoryLogHandle* NewHandleLocked(MemoryRepository* repository,
                                 MemorySession* session, bool writable) {
  MemoryLogHandle* handle = ArenaPushStruct(&repository->Mem, MemoryLogHandle);
  *handle = MemoryLogHandle{};
  handle->Repository = repository;
  handle->Session = session;
  handle->Writable = writable;
  if (writable) session->Writer = handle;
  return handle;
}

SessionLog LogInterface(MemoryLogHandle* handle);

SessionResult CloneRecords(Arena* arena, const Array<SessionRecord>* source,
                           Array<SessionRecord>* out) {
  Array<SessionRecord> records =
      Array_Make<SessionRecord>(arena, source->Count ? source->Count : 1);
  for (u32 i = 0; i < source->Count; i++) {
    SessionRecord clone = {};
    SessionResult copied = SessionRecord_Clone(arena, &source->Data[i], &clone);
    if (!SessionResult_IsOk(copied)) return copied;
    Array_Push(&records, clone);
  }
  *out = records;
  return SessionResult_Ok(source->Count ? source->Data[source->Count - 1].Seq : 0);
}

// Callers should prefer folded SessionReduction state over ReadAll for any
// per-turn/hot-path need; this exists for callers that genuinely need a full
// snapshot (e.g. export, or the one-time load at harness Init).
SessionResult ReadAll(void* context, Arena* arena, Array<SessionRecord>* out) {
  MemoryLogHandle* handle = (MemoryLogHandle*)context;
  if (!handle || !arena || !out) return SessionResult_Invalid("read arguments are invalid");
  MemoryRepository* repository = handle->Repository;
  ScopedLock lock(&repository->Lock);
  if (handle->Closed)
    return SessionResult_Error(SessionStatus::InvalidState, SVL("session log is closed"));
  repository->ReadAllCalls++;
  return CloneRecords(arena, &handle->Session->Records, out);
}

SessionResult PrepareRecord(const MemorySession* session, u64 seq,
                            const SessionRecord* input, SessionRecord* out) {
  if (!input || !out) return SessionResult_Invalid("append record is null");
  SessionRecord record = *input;
  record.SchemaVersion = SESSION_SCHEMA_VERSION;
  record.SessionId = SessionId(session);
  record.Seq = seq;
  if (!DurableId_IsSet(record.EntryId)) record.EntryId = DurableId_Generate();
  if (record.CreatedAt == 0) record.CreatedAt = NowMillis();
  *out = record;
  return SessionResult_Ok(seq);
}

SessionResult AppendBatch(void* context, SessionRecord* input, u32 count) {
  MemoryLogHandle* handle = (MemoryLogHandle*)context;
  if (!handle || !input || count == 0) return SessionResult_Invalid("append batch is empty");
  MemoryRepository* repository = handle->Repository;
  ScopedLock lock(&repository->Lock);
  if (handle->Closed)
    return SessionResult_Error(SessionStatus::InvalidState, SVL("session log is closed"));
  if (!handle->Writable || handle->Session->Writer != handle)
    return SessionResult_Error(SessionStatus::InvalidState,
                               SVL("session log does not own the writer"));
  if (handle->Faulted)
    return SessionResult_Error(SessionStatus::Faulted, SVL("session log is faulted"));

  repository->AppendCalls++;
  if (repository->FailOnAppendCall != 0 &&
      repository->AppendCalls == repository->FailOnAppendCall) {
    handle->Faulted = true;
    repository->FailOnAppendCall = 0;
    return SessionResult_Error(SessionStatus::IoError,
                               SVL("injected memory repository append failure"));
  }

  MemorySession* session = handle->Session;
  Arena scratch = ArenaCreate();
  Array<SessionRecord> prepared = Array_Make<SessionRecord>(&scratch, count);
  u64 firstSeq = (u64)session->Records.Count + 1;

  // Two-phase: validate every new record against the already-folded
  // reduction (O(1) per record) plus the small in-flight batch prepared so
  // far (bounded by `count`, never the historical log). Only after the
  // whole batch validates do we mutate the persistent records/reduction, so
  // a failure partway through the batch leaves prior history untouched.
  for (u32 i = 0; i < count; i++) {
    SessionRecord record = {};
    SessionResult prep = PrepareRecord(session, firstSeq + i, &input[i], &record);
    if (!SessionResult_IsOk(prep)) {
      ArenaRelease(&scratch);
      return prep;
    }
    SessionResult valid = SessionReducer_ValidateAppend(
        &session->Reduction, &record, prepared.Data, prepared.Count);
    if (!SessionResult_IsOk(valid)) {
      ArenaRelease(&scratch);
      valid.Status = SessionStatus::InvalidArgument;
      return valid;
    }
    Array_Push(&prepared, record);
  }

  for (u32 i = 0; i < prepared.Count; i++) {
    SessionRecord persistent = {};
    SessionResult copied =
        SessionRecord_Clone(&repository->Mem, &prepared.Data[i], &persistent);
    if (!SessionResult_IsOk(copied)) {
      ArenaRelease(&scratch);
      handle->Faulted = true;
      return SessionResult_Error(SessionStatus::Faulted,
                                 SVL("memory repository clone failed"));
    }
    Array_Push(&session->Records, persistent);
    SessionReducer_Fold(&session->Reduction, session->Records.Data, session->Records.Count);
    input[i] = persistent;
  }
  ArenaRelease(&scratch);
  return SessionResult_Ok(LastSeq(session));
}

SessionResult Append(void* context, SessionRecord* record) {
  return AppendBatch(context, record, 1);
}

void CloseLog(void* context) {
  MemoryLogHandle* handle = (MemoryLogHandle*)context;
  if (!handle || handle->Closed) return;
  MemoryRepository* repository = handle->Repository;
  ScopedLock lock(&repository->Lock);
  if (handle->Session->Writer == handle) handle->Session->Writer = nullptr;
  handle->Closed = true;
}

SessionLog LogInterface(MemoryLogHandle* handle) {
  SessionLog log = {};
  log.Ctx = handle;
  log.Writable = handle->Writable;
  log.ReadAll = ReadAll;
  log.Append = Append;
  log.AppendBatch = AppendBatch;
  log.Close = CloseLog;
  return log;
}

MemorySession* CreateSessionLocked(MemoryRepository* repository,
                                   const SessionCreateOptions* options) {
  MemorySession* session = ArenaPushStruct(&repository->Mem, MemorySession);
  *session = MemorySession{};
  session->Records = Array_Make<SessionRecord>(&repository->Mem, 32);
  DurableIdIndex_Init(&session->Reduction.Index, &repository->Mem, 32);

  DurableId sessionId = DurableId_IsSet(options->RequestedId)
                            ? options->RequestedId
                            : DurableId_Generate();
  SessionRecord started = {};
  started.SchemaVersion = SESSION_SCHEMA_VERSION;
  started.SessionId = sessionId;
  started.Seq = 1;
  started.EntryId = DurableId_Generate();
  started.Kind = SessionRecordKind::SessionStarted;
  started.CreatedAt = NowMillis();
  started.SessionStarted.WorkingDirectory = options->WorkingDirectory;
  started.SessionStarted.ProjectKey = options->ProjectKey;
  started.SessionStarted.Label = options->Label;
  started.SessionStarted.ForkedFromSessionId = options->ForkedFromSessionId;
  started.SessionStarted.ForkedFromEntryId = options->ForkedFromEntryId;

  SessionRecord persistent = {};
  SessionResult copied = SessionRecord_Clone(&repository->Mem, &started, &persistent);
  if (!SessionResult_IsOk(copied)) return nullptr;
  Array_Push(&session->Records, persistent);
  SessionReducer_Fold(&session->Reduction, session->Records.Data, session->Records.Count);

  Array_Push(&repository->Sessions, session);
  return session;
}

void DiscardLastSession(MemoryRepository* repository, MemorySession* session) {
  if (repository->Sessions.Count &&
      repository->Sessions.Data[repository->Sessions.Count - 1] == session)
    repository->Sessions.Count--;
}

SessionResult Create(void* context, Arena* arena, const SessionCreateOptions* options,
                     SessionLog* outLog, SessionInfo* outInfo) {
  MemoryRepository* repository = (MemoryRepository*)context;
  if (!repository || !arena || !options || !outLog || !outInfo ||
      options->ProjectKey.Length == 0 || options->WorkingDirectory.Length == 0 ||
      (DurableId_IsSet(options->ForkedFromSessionId) !=
       DurableId_IsSet(options->ForkedFromEntryId)))
    return SessionResult_Invalid("create arguments are invalid");
  ScopedLock lock(&repository->Lock);
  if (DurableId_IsSet(options->RequestedId) &&
      IdExists(repository, options->RequestedId))
    return SessionResult_Error(SessionStatus::Conflict, SVL("session ID already exists"));
  MemorySession* session = CreateSessionLocked(repository, options);
  if (!session)
    return SessionResult_Error(SessionStatus::IoError,
                               SVL("failed to initialize memory session"));
  MemoryLogHandle* handle = NewHandleLocked(repository, session, true);
  *outLog = LogInterface(handle);
  SessionResult built = BuildInfo(arena, session, outInfo);
  if (!SessionResult_IsOk(built)) {
    session->Writer = nullptr;
    handle->Closed = true;
    DiscardLastSession(repository, session);
    *outLog = {};
    return built;
  }
  return built;
}

SessionResult Open(void* context, Arena* arena, const SessionOpenOptions* options,
                   SessionLog* outLog, SessionInfo* outInfo) {
  MemoryRepository* repository = (MemoryRepository*)context;
  if (!repository || !arena || !options || !outLog || !outInfo ||
      !DurableId_IsSet(options->SessionId) || options->ProjectKey.Length == 0)
    return SessionResult_Invalid("open arguments are invalid");
  ScopedLock lock(&repository->Lock);
  MemorySession* session =
      FindSession(repository, options->ProjectKey, options->SessionId);
  if (!session)
    return SessionResult_Error(SessionStatus::NotFound, SVL("session was not found"));
  if (options->Writable && session->Writer)
    return SessionResult_Error(SessionStatus::Busy, SVL("session already has a writer"));
  // The in-memory session object is already validated and folded (built at
  // Create time, kept current on every append); no linear re-validation on
  // this Open path.
  MemoryLogHandle* handle = NewHandleLocked(repository, session, options->Writable);
  *outLog = LogInterface(handle);
  SessionResult built = BuildInfo(arena, session, outInfo);
  if (!SessionResult_IsOk(built)) {
    if (session->Writer == handle) session->Writer = nullptr;
    handle->Closed = true;
    *outLog = {};
    return built;
  }
  return built;
}

SessionResult Resolve(void* context, Arena* arena, DurableId sessionId,
                      SessionInfo* outInfo) {
  MemoryRepository* repository = (MemoryRepository*)context;
  if (!repository || !arena || !outInfo || !DurableId_IsSet(sessionId))
    return SessionResult_Invalid("resolve arguments are invalid");
  ScopedLock lock(&repository->Lock);
  MemorySession* found = nullptr;
  for (u32 i = 0; i < repository->Sessions.Count; i++) {
    MemorySession* session = repository->Sessions.Data[i];
    if (!DurableId_Eq(SessionId(session), sessionId))
      continue;
    if (found)
      return SessionResult_Error(SessionStatus::Conflict,
                                 SVL("session ID exists in multiple projects"));
    found = session;
  }
  if (!found)
    return SessionResult_Error(SessionStatus::NotFound,
                               SVL("session was not found"));
  return BuildInfo(arena, found, outInfo);
}

SessionResult List(void* context, Arena* arena,
                   const SessionListOptions* options,
                   Array<SessionInfo>* outItems) {
  MemoryRepository* repository = (MemoryRepository*)context;
  if (!repository || !arena || !options || !outItems ||
      (options->Scope != SessionListScope::CurrentProject &&
       options->Scope != SessionListScope::AllProjects) ||
      (options->Scope == SessionListScope::CurrentProject &&
       options->ProjectKey.Length == 0))
    return SessionResult_Invalid("list arguments are invalid");
  if (options->SkippedCount) *options->SkippedCount = 0;
  ScopedLock lock(&repository->Lock);
  Array<SessionInfo> items =
      Array_Make<SessionInfo>(arena, repository->Sessions.Count
                                        ? repository->Sessions.Count
                                        : 1);
  for (u32 i = repository->Sessions.Count; i > 0; i--) {
    MemorySession* session = repository->Sessions.Data[i - 1];
    if (options->Scope == SessionListScope::CurrentProject &&
        !SessionProjectKey_Eq(ProjectKey(session), options->ProjectKey))
      continue;
    SessionInfo info = {};
    SessionResult built = BuildInfo(arena, session, &info);
    if (!SessionResult_IsOk(built)) return built;
    Array_Push(&items, info);
  }
  SessionInfo_SortByActivity(&items);
  *outItems = items;
  return SessionResult_Ok();
}

SessionResult Fork(void* context, Arena* arena, const SessionForkOptions* options,
                   SessionLog* outLog, SessionInfo* outInfo) {
  MemoryRepository* repository = (MemoryRepository*)context;
  if (!repository || !arena || !options || !outLog || !outInfo ||
      !DurableId_IsSet(options->SourceSessionId) ||
      !DurableId_IsSet(options->SourceHeadId) || options->ProjectKey.Length == 0 ||
      options->WorkingDirectory.Length == 0)
    return SessionResult_Invalid("fork arguments are invalid");
  ScopedLock lock(&repository->Lock);
  MemorySession* source =
      FindSession(repository, options->ProjectKey, options->SourceSessionId);
  if (!source)
    return SessionResult_Error(SessionStatus::NotFound,
                               SVL("source session was not found"));
  if (DurableId_IsSet(options->RequestedTargetId) &&
      IdExists(repository, options->RequestedTargetId))
    return SessionResult_Error(SessionStatus::Conflict,
                               SVL("target session ID already exists"));

  Arena scratch = ArenaCreate();
  Array<const SessionRecord*> branch = {};
  SessionResult ancestry = SessionReducer_Ancestry(
      &source->Reduction, &scratch, options->SourceHeadId, &branch);
  if (!SessionResult_IsOk(ancestry)) {
    ArenaRelease(&scratch);
    return ancestry;
  }

  SessionCreateOptions create = {};
  create.RequestedId = options->RequestedTargetId;
  create.ProjectKey = options->ProjectKey;
  create.WorkingDirectory = options->WorkingDirectory;
  create.Label = options->Label;
  create.ForkedFromSessionId = options->SourceSessionId;
  create.ForkedFromEntryId = options->SourceHeadId;
  MemorySession* target = CreateSessionLocked(repository, &create);
  if (!target) {
    ArenaRelease(&scratch);
    return SessionResult_Error(SessionStatus::IoError,
                               SVL("failed to initialize fork"));
  }

  Array<DurableId> oldIds = Array_Make<DurableId>(&scratch, branch.Count);
  Array<DurableId> newIds = Array_Make<DurableId>(&scratch, branch.Count);
  Array_Push(&oldIds, branch.Data[0]->EntryId);
  Array_Push(&newIds, target->Records.Data[0].EntryId);

  for (u32 i = 1; i < branch.Count; i++) {
    const SessionRecord* sourceRecord = branch.Data[i];
    SessionRecord copy = {};
    SessionResult prepared = SessionRecord_PrepareForkCopy(
        sourceRecord, SessionId(target), target->Records.Count + 1,
        NowMillis(), &oldIds, &newIds, &copy);
    if (!SessionResult_IsOk(prepared)) {
      DiscardLastSession(repository, target);
      ArenaRelease(&scratch);
      return prepared;
    }

    SessionRecord persistent = {};
    SessionResult cloned =
        SessionRecord_Clone(&repository->Mem, &copy, &persistent);
    if (!SessionResult_IsOk(cloned)) {
      DiscardLastSession(repository, target);
      ArenaRelease(&scratch);
      return cloned;
    }
    Array_Push(&target->Records, persistent);
    SessionReducer_Fold(&target->Reduction, target->Records.Data, target->Records.Count);
    Array_Push(&oldIds, sourceRecord->EntryId);
    Array_Push(&newIds, persistent.EntryId);
  }

  MemoryLogHandle* handle = NewHandleLocked(repository, target, true);
  *outLog = LogInterface(handle);
  SessionResult built = BuildInfo(arena, target, outInfo);
  if (!SessionResult_IsOk(built)) {
    target->Writer = nullptr;
    handle->Closed = true;
    DiscardLastSession(repository, target);
    *outLog = {};
    ArenaRelease(&scratch);
    return built;
  }
  ArenaRelease(&scratch);
  return SessionResult_Ok(LastSeq(target));
}

void CloseRepository(void* context) {
  MemoryRepository_Destroy((MemoryRepository*)context);
}

}  // namespace

void MemoryRepository_Init(MemoryRepository* repository) {
  if (!repository) return;
  *repository = MemoryRepository{};
  repository->Mem = ArenaCreate();
  repository->Sessions = Array_Make<MemorySession*>(&repository->Mem, 8);
  Mutex_Init(&repository->Lock);
  repository->Initialized = true;
}

void MemoryRepository_Destroy(MemoryRepository* repository) {
  if (!repository || !repository->Initialized) return;
  Mutex_Destroy(&repository->Lock);
  ArenaRelease(&repository->Mem);
  *repository = MemoryRepository{};
}

SessionRepository MemoryRepository_AsRepository(MemoryRepository* repository) {
  SessionRepository result = {};
  result.Ctx = repository;
  result.Create = Create;
  result.Open = Open;
  result.Resolve = Resolve;
  result.List = List;
  result.Fork = Fork;
  result.Close = CloseRepository;
  return result;
}

void MemoryRepository_FailAfterAppends(MemoryRepository* repository,
                                       u32 successfulCalls) {
  if (!repository || !repository->Initialized) return;
  ScopedLock lock(&repository->Lock);
  repository->FailOnAppendCall =
      repository->AppendCalls + (u64)successfulCalls + 1;
}

void MemoryRepository_ClearFailure(MemoryRepository* repository) {
  if (!repository || !repository->Initialized) return;
  ScopedLock lock(&repository->Lock);
  repository->FailOnAppendCall = 0;
}
