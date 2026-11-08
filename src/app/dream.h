#pragma once

#include "agent/harness.h"
#include "app/wiki.h"
#include "session/repository.h"

// Dreaming: an offline pass in which an agent replays what recent sessions did
// and corrects, extends, merges, and prunes the repository's memory wiki.
//
// `clanker --dream` runs one pass for the working directory's repository;
// `/dream` starts that command in the background and `/dream watch` follows it
// through its progress file and session log. A pass reads the journal for
// sessions that worked in the repository since the last pass, renders the new
// part of each transcript as a markdown digest in `.dream/inbox/`, snapshots
// `pages/`, and runs a headless agent whose working directory is the wiki and
// whose writes are confined to `pages/`. Lint errors left behind get up to two
// follow-up turns. Only a pass that finishes advances `.dream/state.json`, so
// a failed pass is retried from the same point.

constexpr u32 DREAM_INBOX_MAX_BYTES = 2u << 20;
constexpr u32 DREAM_MESSAGE_MAX_BYTES = 16u << 10;
constexpr u32 DREAM_TOOL_RESULT_MAX_BYTES = 1500;
constexpr u32 DREAM_TOOL_ARGS_MAX_BYTES = 400;
constexpr u32 DREAM_SNAPSHOTS_KEPT = 3;
constexpr u32 DREAM_LINT_FOLLOW_UPS = 2;
// A session journaled within this window is checked for new entries by every
// pass even when the journal has nothing new about it. Sessions re-journal at
// most daily while they are used, so a long-lived one stays inside it.
constexpr u64 DREAM_RECHECK_MS = 7ull * 24 * 60 * 60 * 1000;
// Watermarks older than this are forgotten.
constexpr u64 DREAM_FORGET_MS = 90ull * 24 * 60 * 60 * 1000;

// Substitutes `{{inbox}}`, `{{pages}}`, `{{repo}}`, `{{page_limit}}`, and
// `{{pinned_limit}}` in a dreaming system prompt, built-in or DREAM.md.
StrView Dream_ExpandPrompt(Arena* arena, StrView prompt, const WikiLocation* location);

// ---- digests --------------------------------------------------------------

struct DreamDigest {
  // Empty when the range holds nothing worth replaying.
  StrView Markdown;
  u64 FirstSeq;
  // The new watermark: the last record covered.
  u64 LastSeq;
  // Stopped at `maxBytes`; the rest waits for the next pass.
  bool Truncated;
};

// Renders the records after `afterSeq` -- user and assistant text, compact
// tool calls, tool errors in full and other results clipped, compaction
// summaries -- within `maxBytes`. Reasoning is never included.
void Dream_RenderDigest(Arena* arena, const SessionRecord* records, u32 count,
                        u64 afterSeq, StrView sessionId, StrView workingDirectory,
                        u32 maxBytes, DreamDigest* out);

// ---- state ----------------------------------------------------------------

struct DreamSessionState {
  StrView SessionId;
  StrView ProjectKey;
  StrView WorkingDirectory;
  u64 Seq;
  u64 SeenMs;
};

struct DreamState {
  u64 JournalOffset;
  Array<DreamSessionState> Sessions;
};

// A missing file is the empty state; an unreadable one is an error so a pass
// never silently starts over.
bool DreamState_Load(Arena* arena, StrView path, DreamState* out);
bool DreamState_Save(Arena* arena, StrView path, const DreamState* state);

// ---- running ----------------------------------------------------------------

struct DreamRun {
  WikiLocation Location;
  // Where journaled sessions are read from, and where the pass's own session
  // is written. Usually the same repository.
  SessionRepository* Sources;
  SessionRepository* Sessions;
  Provider ProviderImpl;
  Model ModelInfo;
  ModelRequestProfile Profile;
  void* AuthUser;
  AgentResolveAuthFn ResolveAuth;
  AgentPermissionReviewer Reviewer;
  StrView ReviewerModel;
  f64 ReviewerThreshold;
  AgentRetryPolicy Retry;
  AgentTurnLimitPolicy TurnLimit;
  // Lets a long pass fold its own history instead of overflowing the window.
  AgentCompactionPolicy Compaction;
  StrView SystemPrompt;
  AgentHarnessEventObserver Events;
  u64 NowMs;
};

enum class DreamOutcome : u8 { Dreamed, NothingToDo, Busy, Failed };

struct DreamResult {
  DreamOutcome Outcome;
  u32 Digests;
  u32 PagesChanged;
  DurableId SessionId;
  // The agent's closing report, or why the pass did not run.
  StrView Message;
};

// One pass. Changes the process working directory for the duration of the run
// and restores it. Results are owned by `arena`.
void Dream_Execute(Arena* arena, const DreamRun* run, DreamResult* out);

// ---- progress ---------------------------------------------------------------
// `.dream/progress.json` describes the newest pass as it advances, so another
// process can follow it. Only the pass holding the dream lock writes it, always
// atomically, and its last write lands before the lock is released.

enum class DreamPhase : u8 { Preparing, Dreaming, Finished };

struct DreamProgress {
  // The pass's DreamRun::NowMs.
  u64 StartedMs;
  DreamPhase Phase;
  // From Dreaming on: the pass's own session.
  StrView SessionId;
  StrView ProjectKey;
  // Finished only.
  DreamOutcome Outcome;
  u32 Digests;
  u32 PagesChanged;
  StrView Message;
};

// False when the file is missing or unreadable.
bool DreamProgress_Load(Arena* arena, StrView path, DreamProgress* out);
bool DreamProgress_Save(Arena* arena, StrView path, const DreamProgress* progress);

// ---- watching ---------------------------------------------------------------

enum class DreamWatchState : u8 {
  // No pass has reported yet.
  Starting,
  // Rendering digests and taking the snapshot.
  Preparing,
  // The agent is running in the progress's session.
  Dreaming,
  // The pass ended; the progress says how.
  Finished,
  // The pass let go of the lock without finishing, so it died.
  Stopped,
  // Nothing reported within DREAM_WATCH_START_TIMEOUT_MS.
  NotStarted,
};

constexpr u64 DREAM_WATCH_START_TIMEOUT_MS = 20000;

// Where the followed pass stands. `progress` is null when none was read.
// `sinceMs` is when the watcher started the pass it waits for, so progress
// from before then belongs to an earlier pass; 0 follows whichever pass holds
// the lock.
DreamWatchState Dream_WatchState(bool running, const DreamProgress* progress,
                                 u64 sinceMs, u64 nowMs);

// Whether a pass holds the lock, then its progress -- in that order, so a pass
// seen without the lock is seen finished too. False when no progress was read.
bool Dream_ReadProgress(Arena* arena, const WikiLocation* location, bool* running,
                        DreamProgress* out);

// The messages the pass's session has recorded so far, in log order. Fails
// with NotFound or Busy until the session writes its first message.
SessionResult Dream_ReadMessages(Arena* arena, SessionRepository* sessions,
                                 const DreamProgress* progress, Array<Message>* out);

struct DreamCommandLine {
  StrView WorkingDirectory;
  StrView Store;
  StrView DataRoot;
  StrView AgentDirectory;
  StrView SettingsPath;
  StrView AuthPath;
  StrView TrustPath;
  const char* ModelOverride;
};

// `clanker --dream`. Returns the process exit code.
int Dream_RunCommandLine(Arena* arena, const DreamCommandLine* options);

// Starts `clanker --dream` for `workingDirectory` in the background.
bool Dream_SpawnBackground(Arena* arena, StrView workingDirectory);

enum class DreamRestoreResult : u8 { Restored, NoSnapshot, Busy, Failed };

// Replaces `pages/` with the newest snapshot, refusing while a pass runs.
// `*restored` is the snapshot's page count, `*snapshot` its name.
DreamRestoreResult Dream_RestoreLatestSnapshot(Arena* arena,
                                               const WikiLocation* location,
                                               u32* restored, StrView* snapshot);

struct DreamStatus {
  bool Running;
  bool HasLastDream;
  WikiJournalEntry LastDream;
  u32 PendingSessions;
  u32 Snapshots;
};

void Dream_ReadStatus(Arena* arena, const WikiLocation* location, DreamStatus* out);
