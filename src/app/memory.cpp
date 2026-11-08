#include "app/memory.h"

#include "tools/fs.h"

namespace {

bool Active(const MemoryExtension* memory) {
  return memory->Enabled && memory->Pages.Length;
}

void ApplyWritableRoots(MemoryExtension* memory) {
  if (!memory->Harness) return;
  AgentHarness_SetWritableRoots(memory->Harness, memory->WritableRoots,
                                Active(memory) ? 1 : 0);
}

void AppendNote(Arena* arena, ToolOutcome* io, StrView note) {
  Array<Content> content = Array_Make<Content>(arena, io->Content.Count + 1);
  for (u32 i = 0; i < io->Content.Count; i++) Array_Push(&content, io->Content.Data[i]);
  Array_Push(&content, Content_Text(note));
  io->Content = content;
}

void SessionChanged(void* user, const AppSessionContext* session) {
  MemoryExtension* memory = (MemoryExtension*)user;
  ArenaReset(&memory->Mem);
  memory->Location = {};
  memory->Root = {};
  memory->Pages = {};
  memory->WritableRoots[0] = {};
  memory->Session = {};
  memory->JournaledMs = 0;
  memory->WorkingDirectory = StrView_Dup(&memory->Mem, session->WorkingDirectory);
  if (Wiki_Locate(&memory->Mem, memory->Store, memory->WorkingDirectory,
                  &memory->Location) &&
      Fs_MkdirP(&memory->Mem, memory->Location.Pages)) {
    memory->Root = Fs_PermissionPath(&memory->Mem, memory->Location.Root,
                                     memory->Location.Root);
    memory->Pages = Fs_PermissionPath(&memory->Mem, memory->Location.Root,
                                      memory->Location.Pages);
    memory->WritableRoots[0] = memory->Pages;
  }
  if (session->SessionId.Length) {
    memory->Session.Kind = WikiJournalKind::Session;
    memory->Session.SessionId = StrView_Dup(&memory->Mem, session->SessionId);
    memory->Session.ProjectKey = StrView_Dup(&memory->Mem, session->ProjectKey);
    memory->Session.WorkingDirectory = memory->WorkingDirectory;
    memory->Session.TranscriptPath =
        StrView_Dup(&memory->Mem, session->TranscriptPath);
  }
  ApplyWritableRoots(memory);
}

// Journaled at the first prompt rather than at startup: a session that never
// receives one is never written, and dreaming has nothing to read from it.
SessionResult BeforeRun(void* user, Arena* arena, Message*) {
  MemoryExtension* memory = (MemoryExtension*)user;
  u64 now = NowMillis();
  if (Active(memory) && memory->Session.SessionId.Length &&
      (!memory->JournaledMs || now - memory->JournaledMs >= MEMORY_REJOURNAL_MS)) {
    memory->Session.TimeMs = now;
    if (WikiJournal_Append(arena, memory->Location.Journal, &memory->Session))
      memory->JournaledMs = now;
  }
  return SessionResult_Ok();
}

void BeforeTool(void* user, Arena* arena, StrView toolName, StrView,
                const ToolArgs* args, AgentToolGateDecision* out) {
  MemoryExtension* memory = (MemoryExtension*)user;
  if (!Active(memory)) return;
  StrView target = Fs_MutationTarget(arena, toolName, args, memory->WorkingDirectory);
  if (!target.Length || !Fs_PathContained(memory->Root, target) ||
      Fs_PathContained(memory->Pages, target))
    return;
  StrBuf reason = StrBuf_Make(arena, memory->Pages.Length + 160);
  StrBuf_AppendCStr(&reason,
                    "The memory wiki's journal and dreaming state belong to "
                    "Clanker. Write wiki pages under ");
  StrBuf_Append(&reason, Fs_ToPosix(arena, memory->Pages));
  StrBuf_AppendCStr(&reason, " instead.");
  out->Block = true;
  out->Reason = StrBuf_View(&reason);
}

void AfterTool(void* user, Arena* arena, StrView toolName, StrView toolCallId,
               const ToolArgs* args, ToolOutcome* io) {
  MemoryExtension* memory = (MemoryExtension*)user;
  if (!Active(memory) || io->IsError) return;
  StrView target = Fs_MutationTarget(arena, toolName, args, memory->WorkingDirectory);
  if (!target.Length || Fs_PathEquals(target, memory->Pages) ||
      !Fs_PathContained(memory->Pages, target))
    return;

  StrView name = Fs_Base(target);
  if (!Fs_PathEquals(Fs_Parent(target), memory->Pages) || name.Length <= 3 ||
      !StrView_EndsWith(name, SVL(".md"))) {
    AppendNote(arena, io,
               SVL("Memory wiki: only <slug>.md files directly in the pages "
                   "folder are part of the wiki, so this file is ignored."));
    return;
  }

  StrView slug = {name.Data, name.Length - 3};
  Wiki wiki = {};
  Wiki_Scan(arena, memory->Pages, &wiki);
  const WikiPage* written = Wiki_Find(&wiki, slug);
  Array<WikiIssue> issues = Array_Make<WikiIssue>(arena, 8);
  if (written) Wiki_LintPage(arena, &wiki, written, &issues);
  if (issues.Count) {
    StrBuf note = StrBuf_Make(arena, 256);
    StrBuf_AppendCStr(&note, "Memory wiki: ");
    StrBuf_Append(&note, slug);
    StrBuf_AppendCStr(&note, ".md was saved but needs fixing:\n");
    Wiki_AppendIssues(&note, &issues);
    AppendNote(arena, io, StrBuf_View(&note));
  }

  WikiJournalEntry entry = {};
  entry.Kind = WikiJournalKind::Page;
  entry.TimeMs = NowMillis();
  entry.SessionId = memory->Session.SessionId;
  entry.Slug = slug;
  entry.ToolCallId = toolCallId;
  WikiJournal_Append(arena, memory->Location.Journal, &entry);
}

void Shutdown(void* user) {
  MemoryExtension* memory = (MemoryExtension*)user;
  if (memory->Harness) AgentHarness_SetWritableRoots(memory->Harness, nullptr, 0);
  ArenaRelease(&memory->Mem);
}

}  // namespace

void MemoryExtension_Init(MemoryExtension* memory, AgentHarness* harness,
                          StrView store, bool enabled) {
  *memory = MemoryExtension{};
  memory->Harness = harness;
  memory->Store = store;
  memory->Enabled = enabled;
  memory->Mem = ArenaCreate((usize)16 << 20);
}

AppExtension MemoryExtension_Make(MemoryExtension* memory) {
  AppExtension extension = {};
  extension.User = memory;
  extension.Hooks.BeforeRun = BeforeRun;
  extension.Hooks.BeforeTool = BeforeTool;
  extension.Hooks.AfterTool = AfterTool;
  extension.SessionChanged = SessionChanged;
  extension.Shutdown = Shutdown;
  return extension;
}

void MemoryExtension_SetEnabled(MemoryExtension* memory, bool enabled) {
  if (!memory) return;
  memory->Enabled = enabled;
  ApplyWritableRoots(memory);
}
