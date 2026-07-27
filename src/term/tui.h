#pragma once
#include "agent/event.h"
#include "base/arena.h"
#include "base/thread.h"
#include "term/keys.h"
#include "term/lineedit.h"
#include "term/linemodel.h"
#include "term/screen.h"
#include "term/stream_reveal.h"
#include "term/term.h"

// Full-screen TUI controller. Owns the terminal, a differential renderer, the
// transcript line model and the multi-line input editor. Runs two background
// threads (input + render); the caller's thread drives the agent loop and feeds
// events through Tui_AgentEvent / drains user input via Tui_WaitForSubmit.

enum class TuiState : u8 { Idle, Thinking, Streaming, Tool };

enum class TuiModalKind : u8 { None, Select, Text };
enum class TuiModalResult : u8 { None, Pending, Confirmed, Cancelled };

enum class TuiSubmissionKind : u8 { Prompt, Command };

struct TuiSubmission {
  TuiSubmissionKind Kind;
  StrView Text;
};

enum class TuiDisplayEventKind : u8 {
  Text,
  MessageEnd,
  ToolStart,
  ToolEnd,
  User,
  Notice,
  Error,
  AgentEnd,
};

struct TuiDisplayEvent {
  TuiDisplayEventKind Kind;
  StrBuf Payload;
  StrView ToolCallId;
  StrView ToolName;
  StrView ToolArgsJson;
  StopReason Stop;
  Usage Use;
  u64 Timestamp;
  bool IsError;
};

struct Agent;  // forward decl; the TUI pushes steering into the agent's queues

struct TuiOpts {
  StrView ModelName;
  bool Color;
};

struct Tui {
  Arena* Mem;
  // Resettable conversation/UI arena. Terminal, screen, synchronization, cwd,
  // branch, and current model survive Tui_ClearSession.
  Arena SessionMem;
  Arena ScreenMem;
  Agent* Ag;  // for pushing mid-turn steering into the agent's queues
  Term Trm;
  Screen Scr;
  LineModel Model;
  LineEdit Edit;
  LineEdit ModalEdit;
  KeyDecoder Dec;

  Mutex Lock;
  CondVar SubmitCv;
  CondVar ModalCv;
  CondVar RenderCv;
  Thread InputThread;
  Thread RenderThread;
  bool ThreadsUp;
  bool SyncUp;
  bool SessionUp;
  bool ScreenUp;

  // Main-thread submission queue. Commands are queued even during active turns.
  TuiSubmission Queue[64];
  u32 QHead, QTail, QCount;

  bool Dirty;
  bool Stop;
  bool Quit;
  bool AgentRunning;
  volatile i32 AbortFlag;

  // Status.
  TuiState State;
  StrView StateTool;
  u64 TurnStartMs;
  Usage Use;
  u32 Spinner;
  StrView ModelName;
  u32 QueuedCount;  // messages queued (steer/followUp/nextTurn) per last queue_update
  bool OperationActive;
  StrView OperationLabel;
  StrView OperationStatus;
  volatile i32 OperationAbortFlag;

  // Modal selection / text entry.
  TuiModalKind ModalKind;
  TuiModalResult ModalResult;
  StrView ModalTitle;
  StrView ModalPrompt;
  StrView* ModalOptions;
  u32 ModalOptionCount;
  u32 ModalSelected;
  u32 ModalFirst;
  StrView ModalTextResult;

  // Startup context shown on the sticky line below the prompt.
  StrView Cwd;     // working directory, home-abbreviated for display
  StrView Branch;  // git branch, or empty when not in a repo / detached

  // Scroll.
  bool Follow;
  u32 Top;
  u32 LastTop;
  u16 LastPaneH;

  // Mouse interaction with the last rendered transcript viewport.
  u64 HoveredEntryId;
  u64 PressedEntryId;
  u16 MouseX;
  u16 MouseY;
  bool HasMouse;

  // Received assistant text paced across render frames.
  StreamReveal Reveal;
  Array<TuiDisplayEvent> DisplayEvents;
  u32 DisplayHead;
};

// Enter the alt screen and start threads. Returns false if not a TTY (caller
// should fall back to the plain renderer).
bool Tui_Start(Tui* t, Arena* mem, TuiOpts opts);
void Tui_Stop(Tui* t);

// Non-terminal state init (model, editor, locks). Used by Tui_Start and by
// headless tests that drive the event mapping without a real terminal.
void Tui_InitModel(Tui* t, Arena* mem, TuiOpts opts);

// Clear conversation/UI state at an idle boundary without emitting a notice.
// Call Agent_ResetSession first when Ag may hold views submitted by this TUI.
// Returns false while the agent is active or shutdown is in progress.
bool Tui_ClearSession(Tui* t);

// Push raw input bytes, decode them, and apply the resulting keys under the
// lock. InputLoop uses this; headless tests use it to drive input (scroll,
// editing) without a real terminal.
void Tui_FeedInput(Tui* t, const char* data, u32 n);

// Block until the user submits a prompt/command (returns true) or asks to quit
// (false). Slash-prefixed submissions are commands, including unknown commands.
bool Tui_WaitForSubmit(Tui* t, TuiSubmission* out);

// Add an idle prompt to the visible transcript after application-level preflight
// (such as credential resolution) has accepted it. Atomically commits the turn
// against shutdown and returns false if shutdown already won.
bool Tui_AcceptPrompt(Tui* t, StrView text);

// Thread-safe command UI. These calls only update TUI-owned state; callers must
// perform network, credential, and other blocking work after the call returns.
void Tui_CommandNotice(Tui* t, StrView text);
void Tui_CommandError(Tui* t, StrView text);
void Tui_SetModelName(Tui* t, StrView modelName);
void Tui_BeginOperation(Tui* t, StrView label);
void Tui_SetOperationStatus(Tui* t, StrView status);
// End the operation and atomically return whether cancellation was requested.
bool Tui_EndOperation(Tui* t);
volatile i32* Tui_OperationAbort(Tui* t);

// Non-blocking modal entry points are useful to controllers and headless tests.
// Result remains available until the next modal begins.
bool Tui_BeginSelection(Tui* t, StrView title, const StrView* options, u32 optionCount,
                        u32 initialSelection = 0);
bool Tui_BeginTextEntry(Tui* t, StrView title, StrView prompt, StrView initial = {});
TuiModalResult Tui_GetModalResult(Tui* t, u32* selected, StrView* text);

// Blocking conveniences. Cond_Wait releases the TUI lock, so input and render
// threads remain active while the caller waits.
bool Tui_Select(Tui* t, StrView title, const StrView* options, u32 optionCount,
                u32* selected, u32 initialSelection = 0);
bool Tui_ReadText(Tui* t, StrView title, StrView prompt, StrView initial, StrView* out);

// AgentSink callback (User = Tui*).
void Tui_AgentEvent(void* user, const AgentEvent& ev);

// Advance the paced display pipeline at `nowMs`. Compose uses this under the
// TUI lock; headless tests call it directly when no TUI threads are running.
bool Tui_AdvanceStream(Tui* t, u64 nowMs);
bool Tui_HasPendingDisplay(const Tui* t);

volatile i32* Tui_Abort(Tui* t);
