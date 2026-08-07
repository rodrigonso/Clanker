#include "tui/internal.h"

#include <string.h>

// Transcript hydration and the SessionMem lifecycle. Split out because this
// is where the arena can be released and rebuilt underneath live UI state,
// which is the part of the controller most worth reading on its own.

namespace TuiInternal {

void HydrateTranscriptLocked(Tui* t, const Message* messages,
                                    u32 count) {
  for (u32 i = 0; i < count; i++) {
    const Message* message = &messages[i];
    if (message->Kind == Role::User) {
      if (t->Model.HasAssistant) LineModel_EndAssistant(&t->Model);
      StrBuf text = StrBuf_Make(&t->SessionMem, 128);
      for (u32 c = 0; c < message->Content.Count; c++) {
        const Content* content = &message->Content.Data[c];
        if (content->Kind != ContentKind::Text) continue;
        if (text.Length) StrBuf_AppendChar(&text, '\n');
        StrBuf_Append(&text, content->Text);
      }
      LineModel_AddUser(&t->Model, StrBuf_View(&text), message->Timestamp);
    } else if (message->Kind == Role::Assistant) {
      bool hasText = false;
      for (u32 c = 0; c < message->Content.Count; c++)
        if (message->Content.Data[c].Kind == ContentKind::Text &&
            message->Content.Data[c].Text.Length)
          hasText = true;
      if (hasText) {
        LineModel_BeginAssistant(&t->Model);
        for (u32 c = 0; c < message->Content.Count; c++)
          if (message->Content.Data[c].Kind == ContentKind::Text)
            LineModel_AppendAssistant(&t->Model,
                                      message->Content.Data[c].Text);
        LineModel_EndAssistant(&t->Model);
      }
      for (u32 c = 0; c < message->Content.Count; c++) {
        const Content* content = &message->Content.Data[c];
        if (content->Kind == ContentKind::ToolCall)
          LineModel_AddTool(&t->Model, content->ToolId, content->ToolName,
                            content->ToolArgsJson);
      }
      if (message->Stop == StopReason::Error)
        LineModel_AddNotice(&t->Model, EntryKind::Error,
                            message->ErrorMessage);
      else if (message->Stop == StopReason::Aborted)
        LineModel_AddNotice(&t->Model, EntryKind::Info,
                            SVL("Interrupted by user."));
    } else if (message->Kind == Role::ToolResult) {
      StrBuf text = StrBuf_Make(&t->SessionMem, 128);
      for (u32 c = 0; c < message->Content.Count; c++) {
        const Content* content = &message->Content.Data[c];
        if (content->Kind != ContentKind::Text) continue;
        if (text.Length) StrBuf_AppendChar(&text, '\n');
        StrBuf_Append(&text, content->Text);
      }
      StrView result = StrBuf_View(&text);
      if (!LineModel_FinishTool(&t->Model, message->ToolCallId, result,
                                Truncate(result, 1500), message->IsError))
        LineModel_AddNotice(&t->Model, EntryKind::Error,
                            SVL("tool result did not match a pending call"));
    }
  }
}

// Every Tui field that can hold a string owned by SessionMem and is not rebuilt
// by RebuildTranscriptLocked below. Missing one here is a use-after-free, not a
// leak, so the list is kept next to the swap that depends on it.
void ClearSessionScopedStringsLocked(Tui* t) {
  // Every member is SessionMem-owned by construction, so this is exhaustive
  // without a hand-maintained list -- which is the point of the struct. It used
  // to be nine assignments that a new field could silently escape.
  t->Sess = TuiSessionStrings{};
}

// A modal or a running operation is displaying strings owned by SessionMem and
// is waiting on a caller that will read them again, so the arena cannot move
// out from under it.
bool SessionArenaSwapSafeLocked(const Tui* t) {
  return t->ModalKind == TuiModalKind::None &&
         t->ModalResult != TuiModalResult::Pending && !t->OperationActive;
}

// Rebuilds the transcript from `messages`. `allowSwap` moves it onto a fresh
// SessionMem and releases the old one, which is the only way to reclaim entries
// the scrollback cap has already dropped -- SessionMem is a bump allocator.
//
// Only the compaction path passes true. Lag recovery must not: it runs on the
// event pump thread, concurrently with a main thread that is holding StrViews
// into SessionMem (the submitted prompt from LineEdit_Submit, a modal's text
// result) and has no lock over them. Rebuilding without reclaiming there is
// exactly the pre-existing behaviour, and compaction reclaims it later anyway.
void RebuildTranscriptLocked(Tui* t, const Message* messages, u32 count,
                                    bool allowSwap) {
  u16 width = t->Model.Width;
  u32 cap = t->Model.Cap;
  bool swap = allowSwap && SessionArenaSwapSafeLocked(t);

  Arena next = {};
  TuiSubmission preserved[64] = {};
  u32 preservedCount = 0;
  StrView editText = {};
  u32 editCursor = 0;
  Array<StrView> history = {};
  if (swap) {
    next = ArenaCreate();
    preservedCount = t->QCount;
    for (u32 i = 0; i < preservedCount; i++) {
      const TuiSubmission& queued = t->Queue[(t->QHead + i) % 64];
      preserved[i].Kind = queued.Kind;
      preserved[i].Text = StrView_Dup(&next, queued.Text);
    }
    // Whatever the user has already typed, and their recall history, are theirs
    // -- reclaiming memory must not eat them.
    editText = StrView_Dup(&next, LineEdit_Text(&t->Edit));
    editCursor = LineEdit_Cursor(&t->Edit);
    history = Array_Make<StrView>(&next, t->Edit.History.Count ? t->Edit.History.Count : 1);
    for (u32 i = 0; i < t->Edit.History.Count; i++)
      Array_Push(&history, StrView_Dup(&next, t->Edit.History.Data[i]));
  }

  LineModel_Release(&t->Model);
  if (swap) {
    ArenaRelease(&t->SessionMem);
    t->SessionMem = next;
    ClearSessionScopedStringsLocked(t);
  }
  LineModel_Init(&t->Model, &t->SessionMem, width, cap, t->Color);
  if (swap) {
    LineEdit_Init(&t->Edit, &t->SessionMem);
    LineEdit_Init(&t->ModalEdit, &t->SessionMem);
    t->InputUi = UiTextInputState{&t->Edit, 0, false};
    if (editText.Length) {
      LineEdit_InsertText(&t->Edit, editText);
      LineEdit_SetCursor(&t->Edit, editCursor);
    }
    for (u32 i = 0; i < history.Count; i++)
      Array_Push(&t->Edit.History, history.Data[i]);
    memset(t->Queue, 0, sizeof(t->Queue));
    for (u32 i = 0; i < preservedCount; i++) t->Queue[i] = preserved[i];
    t->QHead = 0;
    t->QTail = preservedCount % 64;
    t->QCount = preservedCount;
  }
  StreamReveal_Init(&t->Reveal, &t->SessionMem);
  t->DisplayEvents = Array_Make<TuiDisplayEvent>(&t->SessionMem, 16);
  t->DisplayHead = 0;
  ResetToolUpdates(t);
  t->Use = {};
  t->Sess.StateTool = {};
  t->State = t->AgentRunning ? TuiState::Thinking : TuiState::Idle;
  HydrateTranscriptLocked(t, messages, count);
  // The live set, for deciding when the next compaction is worth its copy.
  if (swap) t->SessionMemBaseline = t->SessionMem.Used;
}

}  // namespace TuiInternal

using namespace TuiInternal;

void Tui_HydrateTranscript(Tui* t, const Message* messages, u32 count) {
  if (!t || (!messages && count)) return;
  Mutex_Lock(&t->Lock);
  HydrateTranscriptLocked(t, messages, count);
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_ReplaceTranscript(Tui* t, const Message* messages, u32 count) {
  if (!t || (!messages && count)) return;
  Mutex_Lock(&t->Lock);
  RebuildTranscriptLocked(t, messages, count, false);
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

bool Tui_SessionMemoryWantsCompaction(Tui* t) {
  if (!t) return false;
  Mutex_Lock(&t->Lock);
  usize used = t->SessionMem.Used;
  bool worthIt = used > TUI_SESSION_COMPACT_FLOOR &&
                 used > t->SessionMemBaseline * 2;
  bool wants = t->SessionUp && worthIt && !t->AgentRunning && !t->Stop && !t->Quit &&
               !Tui_HasPendingDisplay(t) && SessionArenaSwapSafeLocked(t);
  Mutex_Unlock(&t->Lock);
  return wants;
}

bool Tui_CompactSessionMemory(Tui* t, const Message* messages, u32 count) {
  if (!t || (!messages && count)) return false;
  Mutex_Lock(&t->Lock);
  // Re-check under the lock: the caller sampled the condition before building
  // the transcript, and the input thread runs the whole time.
  if (!t->SessionUp || t->AgentRunning || t->Stop || t->Quit ||
      Tui_HasPendingDisplay(t) || !SessionArenaSwapSafeLocked(t)) {
    Mutex_Unlock(&t->Lock);
    return false;
  }
  RebuildTranscriptLocked(t, messages, count, true);
  t->Follow = true;
  Wake(t);
  Mutex_Unlock(&t->Lock);
  return true;
}