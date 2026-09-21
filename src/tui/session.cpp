#include "tui/internal.h"

#include <string.h>

// Transcript hydration and the SessionMem lifecycle. Split out because this
// is where the arena can be released and rebuilt underneath live UI state,
// which is the part of the controller most worth reading on its own.

namespace TuiInternal {

static StrView HydrationText(Arena* scratch, const Message* message) {
  usize bytes = 0;
  u32 count = 0;
  StrView single = {};
  for (u32 i = 0; i < message->Content.Count; i++) {
    const Content& content = message->Content.Data[i];
    if (content.Kind != ContentKind::Text) continue;
    if (bytes) bytes++;
    bytes += content.Text.Length;
    single = content.Text;
    count++;
  }
  if (count < 2) return single;
  FFI(bytes >= 0xffffffffu);
  StrBuf text = StrBuf_Make(scratch, (u32)bytes + 1);
  for (u32 i = 0; i < message->Content.Count; i++) {
    const Content& content = message->Content.Data[i];
    if (content.Kind != ContentKind::Text) continue;
    if (text.Length) StrBuf_AppendChar(&text, '\n');
    StrBuf_Append(&text, content.Text);
  }
  return StrBuf_View(&text);
}

void HydrateTranscriptLocked(Tui* t, const Message* messages,
                                    u32 count) {
  Arena scratch = ArenaCreate();
  for (u32 i = 0; i < count; i++) {
    ArenaReset(&scratch);
    ArenaTrim(&scratch, (usize)64 << 10);
    const Message* message = &messages[i];
    if (message->Kind == Role::User) {
      if (t->Model.HasAssistant) LineModel_EndAssistant(&t->Model);
      LineModel_AddUser(&t->Model, HydrationText(&scratch, message), message->Timestamp);
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
      StrView result = HydrationText(&scratch, message);
      if (!LineModel_FinishTool(&t->Model, message->ToolCallId, result,
                                Truncate(result, 1500), message->IsError))
        LineModel_AddNotice(&t->Model, EntryKind::Error,
                            SVL("tool result did not match a pending call"));
    }
  }
  LineModel_Layout(&t->Model);
  ArenaRelease(&scratch);
}

// Every Tui field that can hold a string owned by SessionMem and is not rebuilt
// by RebuildTranscriptLocked below. Missing one here is a use-after-free, not a
// leak, so the list is kept next to the swap that depends on it.
void ClearSessionScopedStringsLocked(Tui* t) {
  // Every member is SessionMem-owned by construction, so this is exhaustive
  // without a hand-maintained list -- which is the point of the struct. It used
  // to be nine assignments that a new field could silently escape.
  t->Sess = TuiSessionStrings{};
  SetTransientText(&t->StateTool, {});
  SetTransientText(&t->RetryStatus, {});
  SetTransientText(&t->OperationLabel, {});
  SetTransientText(&t->OperationStatus, {});
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
// into SessionMem (the submitted prompt from LineEdit_Submit) and has no lock
// over them. Rebuilding without reclaiming there is
// exactly the pre-existing behaviour, and compaction reclaims it later anyway.
void RebuildTranscriptLocked(Tui* t, const Message* messages, u32 count,
                                    bool allowSwap) {
  u16 width = t->Model.Width;
  u32 cap = t->Model.Cap;
  bool rounded = t->Model.Rounded;
  bool swap = allowSwap && SessionArenaSwapSafeLocked(t);

  Arena next = {};
  TuiSubmission preserved[64] = {};
  u32 preservedCount = 0;
  if (swap) {
    next = ArenaCreate();
    preservedCount = t->QCount;
    for (u32 i = 0; i < preservedCount; i++) {
      const TuiSubmission& queued = t->Queue[(t->QHead + i) % 64];
      preserved[i] = queued;
      preserved[i].Text = StrView_Dup(&next, queued.Text);
    }
  }

  LineModel_Release(&t->Model);
  if (swap) {
    Arena old = t->SessionMem;
    t->SessionMem = next;
    // Clone against the final arena address, not the stack-local arena handle:
    // every growable buffer must keep its Backing after this function returns.
    LineEdit_Clone(&t->Edit, &t->Edit, &t->SessionMem);
    ArenaRelease(&old);
    ClearSessionScopedStringsLocked(t);
  }
  LineModel_Init(&t->Model, &t->SessionMem, width, cap, t->Color, rounded);
  if (swap) {
    t->InputUi.Edit = &t->Edit;
    memset(t->Queue, 0, sizeof(t->Queue));
    for (u32 i = 0; i < preservedCount; i++) t->Queue[i] = preserved[i];
    t->QHead = 0;
    t->QTail = preservedCount % 64;
    t->QCount = preservedCount;
  }
  ResetDisplay(t);
  t->Use = {};
  SetTransientText(&t->StateTool, {});
  t->State = t->AgentRunning ? TuiState::Thinking : TuiState::Idle;
  HydrateTranscriptLocked(t, messages, count);
  // The live set, for deciding when the next compaction is worth its copy.
  if (swap) t->SessionMemBaseline = t->SessionMem.Used;
}

}  // namespace TuiInternal

using namespace TuiInternal;

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