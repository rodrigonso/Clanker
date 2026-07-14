#pragma once
#include "agent/event.h"
#include "base/arena.h"
#include "base/thread.h"
#include "term/keys.h"
#include "term/lineedit.h"
#include "term/linemodel.h"
#include "term/screen.h"
#include "term/term.h"

// Full-screen TUI controller. Owns the terminal, a differential renderer, the
// transcript line model and the multi-line input editor. Runs two background
// threads (input + render); the caller's thread drives the agent loop and feeds
// events through Tui_AgentEvent / drains user input via Tui_WaitForSubmit.

enum class TuiState : u8 { Idle, Thinking, Streaming, Tool };

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

  // Mouse interaction with the last rendered transcript viewport.
  u64 HoveredEntryId;
  u64 PressedEntryId;
  u16 MouseX;
  u16 MouseY;
  bool HasMouse;

  // Streamed text staged for one re-render per frame.
  StrBuf Pending;
  // Reused scratch for sanitizing tool output before it enters the transcript.
  StrBuf ToolText;
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

volatile i32* Tui_Abort(Tui* t);
