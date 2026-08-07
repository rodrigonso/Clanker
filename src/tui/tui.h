#pragma once
#include "agent/harness.h"
#include "base/arena.h"
#include "base/thread.h"
#include "term/keys.h"
#include "ui/lineedit.h"
#include "tui/linemodel.h"
#include "term/screen.h"
#include "tui/stream_reveal.h"
#include "term/term.h"
#include "ui/widgets.h"

// Full-screen TUI controller. Owns the terminal, a differential renderer, the
// transcript line model and the multi-line input editor. Runs two background
// threads (input + render); the caller's thread drives the agent loop and feeds
// events through Tui_HarnessEvent / drains user input via Tui_WaitForSubmit.

enum class TuiState : u8 { Idle, Thinking, Streaming, Tool, Retry };

enum class TuiModalKind : u8 { None, Select, Text };
enum class TuiModalResult : u8 { None, Pending, Confirmed, Back, Cancelled };

enum class TuiSubmissionKind : u8 { Prompt, Command };

struct TuiSubmission {
  TuiSubmissionKind Kind;
  StrView Text;
};

enum class TuiDisplayEventKind : u8 {
  Text,
  MessageEnd,
  ToolStart,
  ToolUpdate,
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
  u16 ToolUpdateSlot;
  bool IsError;
};

struct TuiToolUpdateSlot {
  StrView ToolCallId;
  StrBuf Payload;
  bool Active;
  bool Queued;
};

enum { TUI_TOOL_UPDATE_SLOT_COUNT = 64 };

struct TuiCommandSpec {
  StrView Name;
  StrView Alias;
  StrView Description;
};

struct TuiOpts {
  StrView ModelName = {};
  ModelRequestProfile Profile = {};
  bool Color = true;
  const TuiCommandSpec* Commands = nullptr;
  u32 CommandCount = 0;
};

struct Tui {
  Arena* Mem;
  // Resettable conversation/UI arena. Terminal, screen, synchronization, cwd,
  // branch, and current model survive Tui_ClearSession.
  Arena SessionMem;
  // SessionMem.Used right after the last compaction, i.e. the size of the live
  // transcript. Compaction waits for the arena to double past it.
  usize SessionMemBaseline;
  Arena ToolUpdateMem;
  Arena ScreenMem;
  AgentHarness* Harness;
  Term Trm;
  Screen Scr;
  UiContext Ui;
  UiTextInputState InputUi;
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
  bool InputThreadUp;
  bool RenderThreadUp;
  bool SyncUp;
  bool SessionUp;
  bool ToolUpdateUp;
  bool ScreenUp;
  bool UiUp;
  bool Color;

  // Main-thread submission queue. Commands are queued even during active turns.
  TuiSubmission Queue[64];
  u32 QHead, QTail, QCount;

  bool Dirty;
  bool InputDirty;
  bool Stop;
  bool Quit;
  bool AgentRunning;
  volatile i32 AbortFlag;
  u64 LastEscapeMs;
  bool EscapeArmed;

  // Status.
  TuiState State;
  StrView StateTool;
  StrView RetryStatus;
  Usage Use;
  u32 Spinner;
  u64 SpinnerStartMs;
  StrView ModelName;
  ModelRequestProfile Profile;
  u32 QueuedCount;  // messages queued (steer/followUp/nextTurn) per last queue_update
  TuiCommandSpec* Commands;
  u32 CommandCount;
  u32 CommandPaletteSelected;
  u32 CommandPaletteFirst;
  bool CommandPaletteDismissed;
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
  char CwdStorage[4096];
  char BranchStorage[512];

  // Scroll.
  bool Follow;
  u32 Top;
  u32 LastTop;
  u16 LastPaneH;
  bool ScrollbarVisible;
  bool ScrollbarDragging;
  u16 LastScrollbarThumbTop;
  u16 LastScrollbarThumbH;
  u16 ScrollbarDragOffset;

  // Mouse interaction with the last rendered transcript viewport.
  u64 HoveredEntryId;
  u64 PressedEntryId;
  u16 MouseX;
  u16 MouseY;
  u16 MouseDownX;
  u16 MouseDownY;
  bool HasMouse;
  bool MouseDragMoved;
  bool TranscriptDragging;

  // Selection state. Transcript points are absolute display-row/cell
  // coordinates in the LineModel revision captured when the drag began.
  u32 TranscriptAnchorRow;
  u32 TranscriptFocusRow;
  u16 TranscriptAnchorCell;
  u16 TranscriptFocusCell;
  u64 TranscriptSelectionRevision;
  u64 TranscriptSelectionHash;
  i32 SelectionEdgeDirection;
  u64 NextSelectionScrollMs;

  // Clipboard text is queued under Lock, then encoded by the render/shutdown path.
  StrBuf ClipboardRaw;
  StrBuf ClipboardOsc;

  // Transient "copied N chars" status-bar confirmation. Armed by the copy key
  // handler; the deadline is assigned and enforced against the compose clock.
  u32 CopyNoticeChars;
  u64 CopyNoticeExpiryMs;  // 0 while armed but not yet stamped by a frame
  bool CopyNoticeActive;

  // Received assistant text coalesced at render-frame boundaries.
  StreamReveal Reveal;
  Array<TuiDisplayEvent> DisplayEvents;
  u32 DisplayHead;
  TuiToolUpdateSlot ToolUpdates[TUI_TOOL_UPDATE_SLOT_COUNT];
};

// Enter the alt screen, paint the first frame synchronously, then start input
// and render threads. Returns false if not a TTY.
bool Tui_Start(Tui* t, Arena* mem, TuiOpts opts);
void Tui_Stop(Tui* t);

// Populate the sticky cwd/branch line after startup is already responsive.
void Tui_LoadStartupContext(Tui* t);
void Tui_SetStartupContext(Tui* t, StrView cwd, StrView branch);

// Non-terminal state init (model, editor, locks). Used by Tui_Start and by
// headless tests that drive the event mapping without a real terminal.
void Tui_InitModel(Tui* t, Arena* mem, TuiOpts opts);

// Clear conversation/UI state at an idle boundary without emitting a notice.
// Returns false while the agent is active or shutdown is in progress.
bool Tui_ClearSession(Tui* t);
void Tui_HydrateTranscript(Tui* t, const Message* messages, u32 count);
void Tui_ReplaceTranscript(Tui* t, const Message* messages, u32 count);

// SessionMem is a bump allocator, so transcript entries dropped by the
// scrollback cap stay resident until the survivors are copied elsewhere.
// `WantsCompaction` samples the threshold and the idle conditions; the caller
// then builds a transcript and hands it to `CompactSessionMemory`, which
// re-checks under the lock and returns false if the moment has passed.
bool Tui_SessionMemoryWantsCompaction(Tui* t);
bool Tui_CompactSessionMemory(Tui* t, const Message* messages, u32 count);

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
void Tui_SetModelProfile(Tui* t, StrView modelName,
                         ModelRequestProfile profile);
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
TuiModalResult Tui_Select(Tui* t, StrView title, const StrView* options,
                         u32 optionCount, u32* selected,
                         u32 initialSelection = 0);
TuiModalResult Tui_ReadText(Tui* t, StrView title, StrView prompt,
                           StrView initial, StrView* out);
void Tui_OpenCommandPalette(Tui* t);

// Harness watch-event callback (User = Tui*). Consumes AgentHarnessEvent
// directly; there is no intermediate agent-event representation.
void Tui_HarnessEvent(void* user, const AgentHarnessEvent* ev);

// Advance the frame-coalesced display pipeline at `nowMs`. Compose uses this under the
// TUI lock; headless tests call it directly when no TUI threads are running.
bool Tui_AdvanceStream(Tui* t, u64 nowMs);
bool Tui_HasPendingDisplay(const Tui* t);

// Headless composition and inspection helpers.
void Tui_ComposeFrame(Tui* t, u16 cols, u16 rows, u64 nowMs = 0);
bool Tui_TestDrainShutdown(Tui* t, ScreenWriter write, void* user);

volatile i32* Tui_Abort(Tui* t);
