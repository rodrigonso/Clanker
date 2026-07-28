#include "session/jsonl_repository.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "base/config_file.h"
#include "json/json.h"
#include "session/reducer.h"
#include "tools/fs.h"

struct JsonlLogHandle {
  JsonlRepository* Repository;
  u32 ClaimIndex;
  ProcessFileLock WriterLock;
  Arena State;
  Array<SessionRecord> Records;
  StrView Path;
  FILE* AppendFile;
  Mutex AppendLock;
  bool Writable;
  bool Faulted;
  bool Closed;
};

namespace {

SessionResult Invalid(const char* message) {
  return SessionResult_Error(SessionStatus::InvalidArgument, SV(message));
}

SessionResult IoError(const char* message) {
  return SessionResult_Error(SessionStatus::IoError, SV(message));
}

bool ValidProjectKey(StrView key) {
  if (key.Length == 0 || key.Length > 200 || StrView_Eq(key, SVL(".")) ||
      StrView_Eq(key, SVL("..")))
    return false;
  for (u32 i = 0; i < key.Length; i++) {
    char c = key.Data[i];
    bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!valid) return false;
  }
  return true;
}

StrView SessionsRoot(Arena* arena, JsonlRepository* repository) {
  StrView sessions = Fs_Join(arena, repository->Root, SVL("sessions"));
  return Fs_Join(arena, sessions, SVL("v1"));
}

StrView ProjectDir(Arena* arena, JsonlRepository* repository, StrView projectKey) {
  return Fs_Join(arena, SessionsRoot(arena, repository), projectKey);
}

StrView SessionFileName(Arena* arena, DurableId sessionId, const char* suffix) {
  StrBuf name = StrBuf_Make(arena, DURABLE_ID_CHAR_COUNT + 8);
  StrBuf_Append(&name, DurableId_View(&sessionId));
  StrBuf_AppendCStr(&name, suffix);
  return StrBuf_View(&name);
}

StrView SessionPath(Arena* arena, JsonlRepository* repository, StrView projectKey,
                    DurableId sessionId) {
  return Fs_Join(arena, ProjectDir(arena, repository, projectKey),
                 SessionFileName(arena, sessionId, ".jsonl"));
}

StrView LockPath(Arena* arena, JsonlRepository* repository, StrView projectKey,
                 DurableId sessionId) {
  return Fs_Join(arena, ProjectDir(arena, repository, projectKey),
                 SessionFileName(arena, sessionId, ".lock"));
}

bool ClaimMatches(const JsonlWriterClaim* claim, StrView projectKey,
                  DurableId sessionId) {
  return claim->Active && DurableId_Eq(claim->SessionId, sessionId) &&
         StrView_Eq(claim->ProjectKey, projectKey);
}

SessionResult AcquireClaim(JsonlRepository* repository, StrView projectKey,
                           DurableId sessionId, u32* outIndex) {
  ScopedLock lock(&repository->Lock);
  for (u32 i = 0; i < repository->Claims.Count; i++) {
    if (ClaimMatches(&repository->Claims.Data[i], projectKey, sessionId))
      return SessionResult_Error(SessionStatus::Busy,
                                 SVL("session already has an in-process writer"));
  }
  for (u32 i = 0; i < repository->Claims.Count; i++) {
    if (!repository->Claims.Data[i].Active) {
      repository->Claims.Data[i].SessionId = sessionId;
      repository->Claims.Data[i].ProjectKey =
          StrView_Dup(&repository->Mem, projectKey);
      repository->Claims.Data[i].Active = true;
      *outIndex = i;
      return SessionResult_Ok();
    }
  }
  JsonlWriterClaim claim = {};
  claim.SessionId = sessionId;
  claim.ProjectKey = StrView_Dup(&repository->Mem, projectKey);
  claim.Active = true;
  Array_Push(&repository->Claims, claim);
  *outIndex = repository->Claims.Count - 1;
  return SessionResult_Ok();
}

void ReleaseClaim(JsonlRepository* repository, u32 index) {
  ScopedLock lock(&repository->Lock);
  if (index < repository->Claims.Count) repository->Claims.Data[index].Active = false;
}

bool ClaimedInProcess(JsonlRepository* repository, StrView projectKey,
                      DurableId sessionId) {
  ScopedLock lock(&repository->Lock);
  for (u32 i = 0; i < repository->Claims.Count; i++)
    if (ClaimMatches(&repository->Claims.Data[i], projectKey, sessionId)) return true;
  return false;
}

SessionResult AcquireWriter(JsonlRepository* repository, Arena* scratch,
                            StrView projectKey, DurableId sessionId,
                            u32* outClaimIndex, ProcessFileLock* outLock) {
  SessionResult claim =
      AcquireClaim(repository, projectKey, sessionId, outClaimIndex);
  if (!SessionResult_IsOk(claim)) return claim;
  ProcessFileLockStatus status =
      ProcessFileLock_TryAcquire(scratch,
                                 LockPath(scratch, repository, projectKey, sessionId),
                                 outLock);
  if (status != ProcessFileLockStatus::Acquired) {
    ReleaseClaim(repository, *outClaimIndex);
    return SessionResult_Error(status == ProcessFileLockStatus::Busy
                                   ? SessionStatus::Busy
                                   : SessionStatus::IoError,
                               status == ProcessFileLockStatus::Busy
                                   ? SVL("session already has a process writer")
                                   : SVL("failed to acquire session writer lock"));
  }
  return SessionResult_Ok();
}

bool SessionBusy(JsonlRepository* repository, StrView projectKey,
                 DurableId sessionId) {
  if (ClaimedInProcess(repository, projectKey, sessionId)) return true;
  Arena scratch = ArenaCreate();
  ProcessFileLock lock = {};
  ProcessFileLockStatus status =
      ProcessFileLock_TryAcquire(&scratch,
                                 LockPath(&scratch, repository, projectKey, sessionId),
                                 &lock);
  if (status == ProcessFileLockStatus::Acquired) ProcessFileLock_Release(&lock);
  ArenaRelease(&scratch);
  return status == ProcessFileLockStatus::Busy;
}

bool WriteAll(FILE* file, StrView bytes) {
  u32 written = 0;
  while (written < bytes.Length) {
    size_t n = fwrite(bytes.Data + written, 1, bytes.Length - written, file);
    if (n == 0) return false;
    written += (u32)n;
  }
  return fflush(file) == 0;
}

bool TruncateFile(Arena* arena, StrView path, u64 size) {
  StrBuf native = StrBuf_Make(arena, path.Length + 1);
  StrBuf_Append(&native, path);
  FILE* file = fopen(StrBuf_CStr(&native), "r+b");
  if (!file) return false;
#if defined(_WIN32)
  bool ok = _chsize_s(_fileno(file), size) == 0;
#else
  bool ok = ftruncate(fileno(file), (off_t)size) == 0;
#endif
  if (fclose(file) != 0) ok = false;
  return ok;
}

bool AppendNewline(Arena* arena, StrView path) {
  StrBuf native = StrBuf_Make(arena, path.Length + 1);
  StrBuf_Append(&native, path);
  FILE* file = fopen(StrBuf_CStr(&native), "ab");
  if (!file) return false;
  bool ok = WriteAll(file, SVL("\n"));
  if (fclose(file) != 0) ok = false;
  return ok;
}

SessionResult LoadRecords(Arena* arena, StrView path, bool repair,
                          Array<SessionRecord>* out) {
  StrView content = {};
  ConfigFileReadResult read = ConfigFile_ReadAll(arena, path, &content);
  if (read == ConfigFileReadResult::Missing)
    return SessionResult_Error(SessionStatus::NotFound, SVL("session file was not found"));
  if (read != ConfigFileReadResult::Success) return IoError("failed to read session file");
  if (content.Length == 0)
    return SessionResult_Error(SessionStatus::Corrupt, SVL("session file is empty"));

  Array<SessionRecord> records = Array_Make<SessionRecord>(arena, 32);
  u32 lineStart = 0;
  u32 lastGoodNewline = 0;
  while (lineStart < content.Length) {
    u32 lineEnd = lineStart;
    while (lineEnd < content.Length && content.Data[lineEnd] != '\n') lineEnd++;
    bool terminated = lineEnd < content.Length;
    StrView line = {content.Data + lineStart, lineEnd - lineStart};
    if (line.Length == 0) {
      if (terminated)
        return SessionResult_Error(SessionStatus::Corrupt,
                                   SVL("session file contains an empty record"));
      break;
    }

    SessionRecord record = {};
    SessionResult decoded = SessionRecord_Decode(arena, line, &record);
    if (!SessionResult_IsOk(decoded)) {
      if (terminated)
        return SessionResult_Error(
            decoded.Status == SessionStatus::UnsupportedVersion
                ? SessionStatus::UnsupportedVersion
                : SessionStatus::Corrupt,
            SVL("session file contains a malformed non-final record"));
      if (Json_Parse(arena, line) != nullptr)
        return SessionResult_Error(
            decoded.Status == SessionStatus::UnsupportedVersion
                ? SessionStatus::UnsupportedVersion
                : SessionStatus::Corrupt,
            SVL("final session record is structurally corrupt"));
      if (records.Count == 0)
        return SessionResult_Error(SessionStatus::Corrupt,
                                   SVL("session file has no valid prefix"));
      if (repair && !TruncateFile(arena, path, lastGoodNewline))
        return IoError("failed to repair torn session tail");
      break;
    }

    Array_Push(&records, record);
    if (terminated) {
      lastGoodNewline = lineEnd + 1;
      lineStart = lineEnd + 1;
    } else {
      if (repair && !AppendNewline(arena, path))
        return IoError("failed to terminate final session record");
      lineStart = content.Length;
    }
  }

  SessionResult valid = SessionRecords_Validate(records.Data, records.Count);
  if (!SessionResult_IsOk(valid)) return valid;
  *out = records;
  return SessionResult_Ok(records.Data[records.Count - 1].Seq);
}

SessionInfo BuildInfo(Arena* arena, const SessionRecord* records, u32 count,
                      bool busy) {
  SessionInfo info = {};
  const SessionRecord* started = &records[0];
  info.SessionId = started->SessionId;
  info.ProjectKey = StrView_Dup(arena, started->SessionStarted.ProjectKey);
  info.WorkingDirectory =
      StrView_Dup(arena, started->SessionStarted.WorkingDirectory);
  info.Label = StrView_Dup(arena, started->SessionStarted.Label);
  info.CreatedAt = started->CreatedAt;
  info.UpdatedAt = records[count - 1].CreatedAt;
  info.LastSeq = records[count - 1].Seq;
  info.Busy = busy;

  Arena scratch = ArenaCreate();
  SessionReduction reduction = {};
  if (SessionResult_IsOk(
          SessionReducer_Reduce(&scratch, records, count, &reduction))) {
    for (u32 i = 0; i < reduction.Branch.Count; i++) {
      if (reduction.Branch.Data[i]->Kind == SessionRecordKind::BranchLabel)
        info.Label =
            StrView_Dup(arena, reduction.Branch.Data[i]->BranchLabelEntry.Label);
    }
  }
  ArenaRelease(&scratch);
  return info;
}

SessionLog LogInterface(JsonlLogHandle* handle);

void DestroyHandle(JsonlLogHandle* handle, bool removeData) {
  if (!handle || handle->Closed) return;
  Mutex_Lock(&handle->AppendLock);
  if (handle->AppendFile) {
    fclose(handle->AppendFile);
    handle->AppendFile = nullptr;
  }
  if (removeData) {
    StrBuf native = StrBuf_Make(&handle->State, handle->Path.Length + 1);
    StrBuf_Append(&native, handle->Path);
    remove(StrBuf_CStr(&native));
  }
  ProcessFileLock_Release(&handle->WriterLock);
  if (handle->Writable)
    ReleaseClaim(handle->Repository, handle->ClaimIndex);
  handle->Closed = true;
  Mutex_Unlock(&handle->AppendLock);
  Mutex_Destroy(&handle->AppendLock);
  ArenaRelease(&handle->State);
}

SessionResult ReadAll(void* context, Arena* arena, Array<SessionRecord>* out) {
  JsonlLogHandle* handle = (JsonlLogHandle*)context;
  if (!handle || !arena || !out) return Invalid("read arguments are invalid");
  ScopedLock lock(&handle->AppendLock);
  if (handle->Closed)
    return SessionResult_Error(SessionStatus::InvalidState, SVL("session log is closed"));
  Array<SessionRecord> records =
      Array_Make<SessionRecord>(arena, handle->Records.Count);
  for (u32 i = 0; i < handle->Records.Count; i++) {
    SessionRecord clone = {};
    SessionResult copied =
        SessionRecord_Clone(arena, &handle->Records.Data[i], &clone);
    if (!SessionResult_IsOk(copied)) return copied;
    Array_Push(&records, clone);
  }
  *out = records;
  return SessionResult_Ok(records.Data[records.Count - 1].Seq);
}

SessionResult AppendBatch(void* context, SessionRecord* input, u32 count) {
  JsonlLogHandle* handle = (JsonlLogHandle*)context;
  if (!handle || !input || count == 0) return Invalid("append batch is empty");
  ScopedLock lock(&handle->AppendLock);
  if (handle->Closed)
    return SessionResult_Error(SessionStatus::InvalidState, SVL("session log is closed"));
  if (!handle->Writable || !handle->AppendFile)
    return SessionResult_Error(SessionStatus::InvalidState,
                               SVL("session log is not writable"));
  if (handle->Faulted)
    return SessionResult_Error(SessionStatus::Faulted, SVL("session log is faulted"));

  Arena scratch = ArenaCreate();
  Array<SessionRecord> candidate =
      Array_Make<SessionRecord>(&scratch, handle->Records.Count + count);
  for (u32 i = 0; i < handle->Records.Count; i++)
    Array_Push(&candidate, handle->Records.Data[i]);
  Array<SessionRecord> prepared = Array_Make<SessionRecord>(&scratch, count);
  u64 firstSeq = (u64)handle->Records.Count + 1;
  for (u32 i = 0; i < count; i++) {
    SessionRecord record = input[i];
    record.SchemaVersion = SESSION_SCHEMA_VERSION;
    record.SessionId = handle->Records.Data[0].SessionId;
    record.Seq = firstSeq + i;
    record.EntryClass = SessionRecord_ClassForKind(record.Kind);
    if (record.CreatedAt == 0) record.CreatedAt = NowMillis();
    Array_Push(&prepared, record);
    Array_Push(&candidate, record);
  }
  SessionResult valid = SessionRecords_Validate(candidate.Data, candidate.Count);
  if (!SessionResult_IsOk(valid)) {
    ArenaRelease(&scratch);
    valid.Status = SessionStatus::InvalidArgument;
    return valid;
  }

  StrBuf bytes = StrBuf_Make(&scratch, count * 512);
  for (u32 i = 0; i < prepared.Count; i++) {
    SessionResult encoded = SessionRecord_Encode(&bytes, &prepared.Data[i]);
    if (!SessionResult_IsOk(encoded)) {
      ArenaRelease(&scratch);
      return encoded;
    }
    StrBuf_AppendChar(&bytes, '\n');
  }
  if (!WriteAll(handle->AppendFile, StrBuf_View(&bytes))) {
    handle->Faulted = true;
    ArenaRelease(&scratch);
    return IoError("failed to append session records");
  }

  for (u32 i = 0; i < prepared.Count; i++) {
    SessionRecord persistent = {};
    SessionResult copied =
        SessionRecord_Clone(&handle->State, &prepared.Data[i], &persistent);
    if (!SessionResult_IsOk(copied)) {
      handle->Faulted = true;
      ArenaRelease(&scratch);
      return SessionResult_Error(SessionStatus::Faulted,
                                 SVL("failed to retain appended session record"));
    }
    Array_Push(&handle->Records, persistent);
    input[i] = prepared.Data[i];
  }
  u64 seq = handle->Records.Data[handle->Records.Count - 1].Seq;
  ArenaRelease(&scratch);
  return SessionResult_Ok(seq);
}

SessionResult Append(void* context, SessionRecord* record) {
  return AppendBatch(context, record, 1);
}

void CloseLog(void* context) { DestroyHandle((JsonlLogHandle*)context, false); }

SessionLog LogInterface(JsonlLogHandle* handle) {
  SessionLog log = {};
  log.Ctx = handle;
  log.Writable = handle->Writable;
  log.ReadAll = ReadAll;
  log.Append = Append;
  log.AppendBatch = AppendBatch;
  log.Close = CloseLog;
  return log;
}

SessionResult InitializeHandle(JsonlRepository* repository, Arena* callerArena,
                               StrView path, bool writable, u32 claimIndex,
                               ProcessFileLock writerLock,
                               JsonlLogHandle** outHandle, SessionInfo* outInfo) {
  JsonlLogHandle* handle = ArenaPushStruct(&repository->Mem, JsonlLogHandle);
  *handle = JsonlLogHandle{};
  handle->Repository = repository;
  handle->ClaimIndex = claimIndex;
  handle->WriterLock = writerLock;
  handle->State = ArenaCreate();
  handle->Path = StrView_Dup(&handle->State, path);
  handle->Writable = writable;
  Mutex_Init(&handle->AppendLock);

  SessionResult loaded =
      LoadRecords(&handle->State, handle->Path, writable, &handle->Records);
  if (!SessionResult_IsOk(loaded)) {
    DestroyHandle(handle, false);
    return loaded;
  }
  if (writable) {
    StrBuf native = StrBuf_Make(&handle->State, handle->Path.Length + 1);
    StrBuf_Append(&native, handle->Path);
    handle->AppendFile = fopen(StrBuf_CStr(&native), "ab");
    if (!handle->AppendFile) {
      DestroyHandle(handle, false);
      return IoError("failed to open session file for append");
    }
  }
  *outInfo = BuildInfo(callerArena, handle->Records.Data, handle->Records.Count,
                       writable);
  *outHandle = handle;
  return loaded;
}

SessionResult Create(void* context, Arena* arena, const SessionCreateOptions* options,
                     SessionLog* outLog, SessionInfo* outInfo) {
  JsonlRepository* repository = (JsonlRepository*)context;
  if (!repository || !arena || !options || !outLog || !outInfo ||
      !ValidProjectKey(options->ProjectKey) ||
      options->WorkingDirectory.Length == 0 ||
      (DurableId_IsSet(options->ForkedFromSessionId) !=
       DurableId_IsSet(options->ForkedFromEntryId)))
    return Invalid("create arguments are invalid");

  Arena scratch = ArenaCreate();
  StrView projectDir = ProjectDir(&scratch, repository, options->ProjectKey);
  if (!Fs_MkdirP(&scratch, projectDir)) {
    ArenaRelease(&scratch);
    return IoError("failed to create session directory");
  }

  DurableId sessionId = options->RequestedId;
  if (!DurableId_IsSet(sessionId)) sessionId = DurableId_Generate();
  StrView path = SessionPath(&scratch, repository, options->ProjectKey, sessionId);
  if (Fs_Exists(path)) {
    ArenaRelease(&scratch);
    return SessionResult_Error(SessionStatus::Conflict,
                               SVL("session file already exists"));
  }

  u32 claimIndex = 0;
  ProcessFileLock writerLock = {};
  SessionResult acquired =
      AcquireWriter(repository, &scratch, options->ProjectKey, sessionId,
                    &claimIndex, &writerLock);
  if (!SessionResult_IsOk(acquired)) {
    ArenaRelease(&scratch);
    return acquired;
  }
  if (Fs_Exists(path)) {
    ProcessFileLock_Release(&writerLock);
    ReleaseClaim(repository, claimIndex);
    ArenaRelease(&scratch);
    return SessionResult_Error(SessionStatus::Conflict,
                               SVL("session file already exists"));
  }

  SessionRecord started = {};
  started.SchemaVersion = SESSION_SCHEMA_VERSION;
  started.SessionId = sessionId;
  started.Seq = 1;
  started.EntryId = DurableId_Generate();
  started.EntryClass = SessionEntryClass::Session;
  started.Kind = SessionRecordKind::SessionStarted;
  started.CreatedAt = NowMillis();
  started.SessionStarted.WorkingDirectory = options->WorkingDirectory;
  started.SessionStarted.ProjectKey = options->ProjectKey;
  started.SessionStarted.Label = options->Label;
  started.SessionStarted.ForkedFromSessionId = options->ForkedFromSessionId;
  started.SessionStarted.ForkedFromEntryId = options->ForkedFromEntryId;

  StrBuf record = StrBuf_Make(&scratch, 512);
  SessionResult encoded = SessionRecord_Encode(&record, &started);
  StrBuf_AppendChar(&record, '\n');
  StrBuf native = StrBuf_Make(&scratch, path.Length + 1);
  StrBuf_Append(&native, path);
  FILE* file = SessionResult_IsOk(encoded) ? fopen(StrBuf_CStr(&native), "wb") : nullptr;
  bool written = file && WriteAll(file, StrBuf_View(&record));
#if !defined(_WIN32)
  if (written && chmod(StrBuf_CStr(&native), 0600) != 0) written = false;
#endif
  if (!written) {
    if (file) fclose(file);
    remove(StrBuf_CStr(&native));
    ProcessFileLock_Release(&writerLock);
    ReleaseClaim(repository, claimIndex);
    ArenaRelease(&scratch);
    return IoError("failed to create session file");
  }

  JsonlLogHandle* handle = ArenaPushStruct(&repository->Mem, JsonlLogHandle);
  *handle = JsonlLogHandle{};
  handle->Repository = repository;
  handle->ClaimIndex = claimIndex;
  handle->WriterLock = writerLock;
  handle->State = ArenaCreate();
  handle->Path = StrView_Dup(&handle->State, path);
  handle->Records = Array_Make<SessionRecord>(&handle->State, 32);
  SessionRecord persistent = {};
  SessionResult copied =
      SessionRecord_Clone(&handle->State, &started, &persistent);
  if (!SessionResult_IsOk(copied)) {
    fclose(file);
    remove(StrBuf_CStr(&native));
    ProcessFileLock_Release(&writerLock);
    ReleaseClaim(repository, claimIndex);
    ArenaRelease(&handle->State);
    ArenaRelease(&scratch);
    return copied;
  }
  Array_Push(&handle->Records, persistent);
  handle->AppendFile = file;
  handle->Writable = true;
  Mutex_Init(&handle->AppendLock);
  *outLog = LogInterface(handle);
  *outInfo = BuildInfo(arena, handle->Records.Data, handle->Records.Count, true);
  ArenaRelease(&scratch);
  return SessionResult_Ok(1);
}

SessionResult Open(void* context, Arena* arena, const SessionOpenOptions* options,
                   SessionLog* outLog, SessionInfo* outInfo) {
  JsonlRepository* repository = (JsonlRepository*)context;
  if (!repository || !arena || !options || !outLog || !outInfo ||
      !ValidProjectKey(options->ProjectKey) ||
      !DurableId_IsSet(options->SessionId))
    return Invalid("open arguments are invalid");
  Arena scratch = ArenaCreate();
  StrView path =
      SessionPath(&scratch, repository, options->ProjectKey, options->SessionId);
  if (!Fs_Exists(path)) {
    ArenaRelease(&scratch);
    return SessionResult_Error(SessionStatus::NotFound,
                               SVL("session file was not found"));
  }

  u32 claimIndex = 0;
  ProcessFileLock writerLock = {};
  if (options->Writable) {
    SessionResult acquired =
        AcquireWriter(repository, &scratch, options->ProjectKey,
                      options->SessionId, &claimIndex, &writerLock);
    if (!SessionResult_IsOk(acquired)) {
      ArenaRelease(&scratch);
      return acquired;
    }
  }

  JsonlLogHandle* handle = nullptr;
  SessionResult initialized =
      InitializeHandle(repository, arena, path, options->Writable, claimIndex,
                       writerLock, &handle, outInfo);
  if (!SessionResult_IsOk(initialized)) {
    ArenaRelease(&scratch);
    return initialized;
  }
  *outLog = LogInterface(handle);
  ArenaRelease(&scratch);
  return initialized;
}

SessionResult Inspect(void* context, Arena* arena, StrView projectKey,
                      DurableId sessionId, SessionInfo* outInfo) {
  JsonlRepository* repository = (JsonlRepository*)context;
  if (!repository || !arena || !outInfo || !ValidProjectKey(projectKey) ||
      !DurableId_IsSet(sessionId))
    return Invalid("inspect arguments are invalid");
  Arena scratch = ArenaCreate();
  StrView path = SessionPath(&scratch, repository, projectKey, sessionId);
  Array<SessionRecord> records = {};
  SessionResult loaded = LoadRecords(&scratch, path, false, &records);
  if (SessionResult_IsOk(loaded))
    *outInfo = BuildInfo(arena, records.Data, records.Count,
                         SessionBusy(repository, projectKey, sessionId));
  ArenaRelease(&scratch);
  return loaded;
}

void SortByUpdated(Array<SessionInfo>* items) {
  for (u32 i = 1; i < items->Count; i++) {
    SessionInfo value = items->Data[i];
    u32 j = i;
    while (j > 0 && items->Data[j - 1].UpdatedAt < value.UpdatedAt) {
      items->Data[j] = items->Data[j - 1];
      j--;
    }
    items->Data[j] = value;
  }
}

SessionResult List(void* context, Arena* arena, StrView projectKey,
                   Array<SessionInfo>* outItems) {
  JsonlRepository* repository = (JsonlRepository*)context;
  if (!repository || !arena || !outItems || !ValidProjectKey(projectKey))
    return Invalid("list arguments are invalid");
  Arena scratch = ArenaCreate();
  StrView dir = ProjectDir(&scratch, repository, projectKey);
  if (!Fs_Exists(dir)) {
    *outItems = Array_Make<SessionInfo>(arena, 1);
    ArenaRelease(&scratch);
    return SessionResult_Ok();
  }
  bool listed = false;
  Array<FsEntry> files = Fs_ListDir(&scratch, dir, &listed);
  if (!listed) {
    ArenaRelease(&scratch);
    return IoError("failed to list session directory");
  }
  Array<SessionInfo> items = Array_Make<SessionInfo>(arena, files.Count ? files.Count : 1);
  for (u32 i = 0; i < files.Count; i++) {
    if (files.Data[i].IsDir || files.Data[i].Name.Length <= 6) continue;
    StrView ext = {files.Data[i].Name.Data + files.Data[i].Name.Length - 6, 6};
    if (!StrView_Eq(ext, SVL(".jsonl"))) continue;
    StrView idText = {files.Data[i].Name.Data, files.Data[i].Name.Length - 6};
    DurableId sessionId = {};
    if (!DurableId_Parse(idText, &sessionId)) continue;

    Arena fileArena = ArenaCreate();
    StrView path = SessionPath(&fileArena, repository, projectKey, sessionId);
    Array<SessionRecord> records = {};
    SessionResult loaded = LoadRecords(&fileArena, path, false, &records);
    if (SessionResult_IsOk(loaded))
      Array_Push(&items,
                 BuildInfo(arena, records.Data, records.Count,
                           SessionBusy(repository, projectKey, sessionId)));
    ArenaRelease(&fileArena);
  }
  SortByUpdated(&items);
  *outItems = items;
  ArenaRelease(&scratch);
  return SessionResult_Ok();
}

DurableId SessionParentId(const SessionRecord* record) {
  switch (record->Kind) {
    case SessionRecordKind::Message: return record->MessageEntry.ParentId;
    case SessionRecordKind::Model: return record->ModelEntry.ParentId;
    case SessionRecordKind::Thinking: return record->ThinkingEntry.ParentId;
    case SessionRecordKind::Tools: return record->ToolsEntry.ParentId;
    case SessionRecordKind::Compaction: return record->CompactionEntry.ParentId;
    case SessionRecordKind::BranchLabel: return record->BranchLabelEntry.ParentId;
    default: return DurableId{};
  }
}

DurableId RemapId(const Array<DurableId>* oldIds, const Array<DurableId>* newIds,
                  DurableId oldId) {
  for (u32 i = 0; i < oldIds->Count; i++)
    if (DurableId_Eq(oldIds->Data[i], oldId)) return newIds->Data[i];
  return DurableId{};
}

SessionResult Fork(void* context, Arena* arena, const SessionForkOptions* options,
                   SessionLog* outLog, SessionInfo* outInfo) {
  JsonlRepository* repository = (JsonlRepository*)context;
  if (!repository || !arena || !options || !outLog || !outInfo ||
      !ValidProjectKey(options->ProjectKey) ||
      !DurableId_IsSet(options->SourceSessionId) ||
      !DurableId_IsSet(options->SourceHeadId) ||
      options->WorkingDirectory.Length == 0)
    return Invalid("fork arguments are invalid");

  Arena scratch = ArenaCreate();
  StrView sourcePath = SessionPath(&scratch, repository, options->ProjectKey,
                                   options->SourceSessionId);
  Array<SessionRecord> sourceRecords = {};
  SessionResult loaded =
      LoadRecords(&scratch, sourcePath, false, &sourceRecords);
  if (!SessionResult_IsOk(loaded)) {
    ArenaRelease(&scratch);
    return loaded;
  }
  SessionReduction reduction = {};
  SessionResult reduced = SessionReducer_Reduce(
      &scratch, sourceRecords.Data, sourceRecords.Count, &reduction);
  if (!SessionResult_IsOk(reduced)) {
    ArenaRelease(&scratch);
    return reduced;
  }
  Array<const SessionRecord*> branch = {};
  SessionResult ancestry = SessionReducer_Ancestry(
      &scratch, &reduction, options->SourceHeadId, &branch);
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
  SessionLog targetLog = {};
  SessionInfo targetInfo = {};
  SessionResult created =
      Create(repository, arena, &create, &targetLog, &targetInfo);
  if (!SessionResult_IsOk(created)) {
    ArenaRelease(&scratch);
    return created;
  }

  Array<SessionRecord> targetRoot = {};
  SessionResult readTarget =
      SessionLog_ReadAll(&targetLog, &scratch, &targetRoot);
  if (!SessionResult_IsOk(readTarget)) {
    DestroyHandle((JsonlLogHandle*)targetLog.Ctx, true);
    ArenaRelease(&scratch);
    return readTarget;
  }
  Array<DurableId> oldIds = Array_Make<DurableId>(&scratch, branch.Count);
  Array<DurableId> newIds = Array_Make<DurableId>(&scratch, branch.Count);
  Array_Push(&oldIds, branch.Data[0]->EntryId);
  Array_Push(&newIds, targetRoot.Data[0].EntryId);

  Array<SessionRecord> copies =
      Array_Make<SessionRecord>(&scratch, branch.Count > 1 ? branch.Count - 1 : 1);
  for (u32 i = 1; i < branch.Count; i++) {
    const SessionRecord* source = branch.Data[i];
    SessionRecord copy = *source;
    copy.EntryId = DurableId_Generate();
    copy.CreatedAt = NowMillis();
    DurableId parent = RemapId(&oldIds, &newIds, SessionParentId(source));
    if (!DurableId_IsSet(parent)) {
      DestroyHandle((JsonlLogHandle*)targetLog.Ctx, true);
      ArenaRelease(&scratch);
      return SessionResult_Error(SessionStatus::Corrupt,
                                 SVL("fork branch parent could not be remapped"));
    }
    switch (copy.Kind) {
      case SessionRecordKind::Message: copy.MessageEntry.ParentId = parent; break;
      case SessionRecordKind::Model: copy.ModelEntry.ParentId = parent; break;
      case SessionRecordKind::Thinking: copy.ThinkingEntry.ParentId = parent; break;
      case SessionRecordKind::Tools: copy.ToolsEntry.ParentId = parent; break;
      case SessionRecordKind::BranchLabel: copy.BranchLabelEntry.ParentId = parent; break;
      case SessionRecordKind::Compaction:
        copy.CompactionEntry.ParentId = parent;
        copy.CompactionEntry.BaseEntryId =
            RemapId(&oldIds, &newIds, copy.CompactionEntry.BaseEntryId);
        copy.CompactionEntry.ThroughEntryId =
            RemapId(&oldIds, &newIds, copy.CompactionEntry.ThroughEntryId);
        copy.CompactionEntry.SourceHeadId =
            RemapId(&oldIds, &newIds, copy.CompactionEntry.SourceHeadId);
        if (!DurableId_IsSet(copy.CompactionEntry.BaseEntryId) ||
            !DurableId_IsSet(copy.CompactionEntry.ThroughEntryId) ||
            !DurableId_IsSet(copy.CompactionEntry.SourceHeadId)) {
          DestroyHandle((JsonlLogHandle*)targetLog.Ctx, true);
          ArenaRelease(&scratch);
          return SessionResult_Error(
              SessionStatus::Corrupt, SVL("fork compaction could not be remapped"));
        }
        break;
      default:
        DestroyHandle((JsonlLogHandle*)targetLog.Ctx, true);
        ArenaRelease(&scratch);
        return SessionResult_Error(SessionStatus::Corrupt,
                                   SVL("fork branch contains a harness record"));
    }
    Array_Push(&copies, copy);
    Array_Push(&oldIds, source->EntryId);
    Array_Push(&newIds, copy.EntryId);
  }

  if (copies.Count) {
    SessionResult appended =
        SessionLog_AppendBatch(&targetLog, copies.Data, copies.Count);
    if (!SessionResult_IsOk(appended)) {
      DestroyHandle((JsonlLogHandle*)targetLog.Ctx, true);
      ArenaRelease(&scratch);
      return appended;
    }
  }
  JsonlLogHandle* handle = (JsonlLogHandle*)targetLog.Ctx;
  *outInfo = BuildInfo(arena, handle->Records.Data, handle->Records.Count, true);
  *outLog = targetLog;
  ArenaRelease(&scratch);
  return SessionResult_Ok(handle->Records.Data[handle->Records.Count - 1].Seq);
}

SessionResult Rename(void* context, DurableId sessionId, StrView projectKey,
                     StrView label) {
  JsonlRepository* repository = (JsonlRepository*)context;
  if (!repository || !DurableId_IsSet(sessionId) ||
      !ValidProjectKey(projectKey) || label.Length == 0)
    return Invalid("rename arguments are invalid");
  Arena scratch = ArenaCreate();
  SessionOpenOptions options = {};
  options.SessionId = sessionId;
  options.ProjectKey = projectKey;
  options.Writable = true;
  SessionLog log = {};
  SessionInfo info = {};
  SessionResult opened = Open(repository, &scratch, &options, &log, &info);
  if (!SessionResult_IsOk(opened)) {
    ArenaRelease(&scratch);
    return opened;
  }
  Array<SessionRecord> records = {};
  SessionResult read = SessionLog_ReadAll(&log, &scratch, &records);
  SessionReduction reduction = {};
  SessionResult reduced =
      SessionResult_IsOk(read)
          ? SessionReducer_Reduce(&scratch, records.Data, records.Count, &reduction)
          : read;
  if (!SessionResult_IsOk(reduced)) {
    SessionLog_Close(&log);
    ArenaRelease(&scratch);
    return reduced;
  }
  SessionRecord rename = {};
  rename.EntryId = DurableId_Generate();
  rename.Kind = SessionRecordKind::BranchLabel;
  rename.BranchLabelEntry.ParentId = reduction.HeadId;
  rename.BranchLabelEntry.Label = label;
  SessionResult appended = SessionLog_Append(&log, &rename);
  SessionLog_Close(&log);
  ArenaRelease(&scratch);
  return appended;
}

SessionResult Delete(void* context, DurableId sessionId, StrView projectKey) {
  JsonlRepository* repository = (JsonlRepository*)context;
  if (!repository || !DurableId_IsSet(sessionId) ||
      !ValidProjectKey(projectKey))
    return Invalid("delete arguments are invalid");
  Arena scratch = ArenaCreate();
  StrView path = SessionPath(&scratch, repository, projectKey, sessionId);
  if (!Fs_Exists(path)) {
    ArenaRelease(&scratch);
    return SessionResult_Error(SessionStatus::NotFound,
                               SVL("session file was not found"));
  }
  u32 claimIndex = 0;
  ProcessFileLock writerLock = {};
  SessionResult acquired =
      AcquireWriter(repository, &scratch, projectKey, sessionId, &claimIndex,
                    &writerLock);
  if (!SessionResult_IsOk(acquired)) {
    ArenaRelease(&scratch);
    return acquired;
  }
  StrBuf native = StrBuf_Make(&scratch, path.Length + 1);
  StrBuf_Append(&native, path);
  bool removed = remove(StrBuf_CStr(&native)) == 0;
  ProcessFileLock_Release(&writerLock);
  ReleaseClaim(repository, claimIndex);
  ArenaRelease(&scratch);
  return removed ? SessionResult_Ok() : IoError("failed to delete session file");
}

void CloseRepository(void* context) {
  JsonlRepository_Destroy((JsonlRepository*)context);
}

}  // namespace

void JsonlRepository_Init(JsonlRepository* repository, StrView dataRoot) {
  if (!repository) return;
  *repository = JsonlRepository{};
  repository->Mem = ArenaCreate();
  repository->Root = StrView_Dup(&repository->Mem, dataRoot);
  repository->Claims = Array_Make<JsonlWriterClaim>(&repository->Mem, 8);
  Mutex_Init(&repository->Lock);
  repository->Initialized = true;
}

void JsonlRepository_Destroy(JsonlRepository* repository) {
  if (!repository || !repository->Initialized) return;
  Mutex_Destroy(&repository->Lock);
  ArenaRelease(&repository->Mem);
  *repository = JsonlRepository{};
}

SessionRepository JsonlRepository_AsRepository(JsonlRepository* repository) {
  SessionRepository result = {};
  result.Ctx = repository;
  result.Create = Create;
  result.Open = Open;
  result.Inspect = Inspect;
  result.List = List;
  result.Fork = Fork;
  result.Rename = Rename;
  result.Delete = Delete;
  result.Close = CloseRepository;
  return result;
}
