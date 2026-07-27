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

enum class TuiDisplayEventKind : u8 {
  Text,
  MessageEnd,
  ToolStart,
  ToolEnd,
  User,
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
  Agent* Ag;  // for pushing mid-turn steering into the agent's queues
  Term Trm;
  Screen Scr;
  LineModel Model;
  LineEdit Edit;
  KeyDecoder Dec;

  Mutex Lock;
  CondVar SubmitCv;
  CondVar RenderCv;
  Thread InputThread;
  Thread RenderThread;
  bool ThreadsUp;

  // Submit queue (user-entered lines awaiting the next prompt, while idle).
  StrView Queue[64];
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

  // Startup context shown on the sticky line below the prompt.
  StrView Cwd;     // working directory, home-abbreviated for display
  StrView Branch;  // git branch, or empty when not in a repo / detached

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

// Push raw input bytes, decode them, and apply the resulting keys under the
// lock. InputLoop uses this; headless tests use it to drive input (scroll,
// editing) without a real terminal.
void Tui_FeedInput(Tui* t, const char* data, u32 n);

// Block until the user submits a line (returns true) or asks to quit (false).
bool Tui_WaitForSubmit(Tui* t, StrView* out);

// AgentSink callback (User = Tui*).
void Tui_AgentEvent(void* user, const AgentEvent& ev);

// Advance the paced display pipeline at `nowMs`. Compose uses this under the
// TUI lock; headless tests call it directly when no TUI threads are running.
bool Tui_AdvanceStream(Tui* t, u64 nowMs);
bool Tui_HasPendingDisplay(const Tui* t);

// Compose one frame at a supplied size. Production rendering queries the
// terminal and calls this; headless tests use it to inspect the back buffer.
void Tui_ComposeFrame(Tui* t, u16 cols, u16 rows);

volatile i32* Tui_Abort(Tui* t);
