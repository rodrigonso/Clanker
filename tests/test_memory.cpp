#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/harness.h"
#include "app/dream.h"
#include "app/memory.h"
#include "app/prompt_context.h"
#include "app/wiki.h"
#include "base/config_file.h"
#include "base/file_io.h"
#include "base/process_file_lock.h"
#include "base/sysinfo.h"
#include "check.h"
#include "json/json.h"
#include "script_provider.h"
#include "session/jsonl_repository.h"
#include "session/memory_repository.h"
#include "session/metadata.h"
#include "tools/filelock.h"
#include "tools/fs.h"
#include "tools/tools.h"

namespace {

// Fixtures live under the OS temp directory: the test binary runs inside the
// Clanker checkout, where every relative path would resolve to its repository.
StrView TempRoot(Arena* arena) {
  const char* base = getenv("TEMP");
  if (!base || !*base) base = getenv("TMPDIR");
  if (!base || !*base) base = "/tmp";
  StrBuf name = StrBuf_Make(arena, 64);
  StrBuf_AppendCStr(&name, "clanker-memory-test-");
  StrBuf_AppendU64(&name, NowMillis());
  StrView root = Fs_Join(arena, SV(base), StrBuf_View(&name));
  Fs_MkdirP(arena, root);
  return Fs_RealPath(arena, root);
}

bool Write(Arena* arena, StrView path, StrView content) {
  return Fs_MkdirP(arena, Fs_Parent(path)) &&
         File_WriteAll(arena, path, content) == FileIoResult::Success;
}

StrView Read(Arena* arena, StrView path) {
  StrView content = {};
  File_ReadAll(arena, path, &content);
  return content;
}

StrView Page(Arena* arena, const char* title, const char* type, const char* summary,
             const char* body, bool pinned = false) {
  StrBuf page = StrBuf_Make(arena, 256);
  StrBuf_AppendCStr(&page, "---\ntitle: ");
  StrBuf_AppendCStr(&page, title);
  StrBuf_AppendCStr(&page, "\ntype: ");
  StrBuf_AppendCStr(&page, type);
  StrBuf_AppendCStr(&page, "\nsummary: ");
  StrBuf_AppendCStr(&page, summary);
  StrBuf_AppendCStr(&page, pinned ? "\npinned: true\n---\n" : "\npinned: false\n---\n");
  StrBuf_AppendCStr(&page, body);
  return StrBuf_View(&page);
}

bool HasIssue(const Array<WikiIssue>* issues, WikiIssueKind kind, const char* slug) {
  for (u32 i = 0; i < issues->Count; i++)
    if (issues->Data[i].Kind == kind && StrView_EqCStr(issues->Data[i].Slug, slug))
      return true;
  return false;
}

u32 CountJournal(Arena* arena, StrView journal, WikiJournalKind kind) {
  Array<WikiJournalEntry> entries = Array_Make<WikiJournalEntry>(arena, 8);
  u64 end = 0;
  WikiJournal_Read(arena, journal, 0, &entries, &end);
  u32 count = 0;
  for (u32 i = 0; i < entries.Count; i++)
    if (entries.Data[i].Kind == kind) count++;
  return count;
}

void TestRepositoryIdentity(Arena* a, StrView root) {
  CHECK_ALL(StrView_EqCStr(Git_CommonDirFromFile(SVL("../..\n")), "../.."),
            StrView_EqCStr(Git_CommonDirFromFile(SVL("  /repo/.git/ \r\n")), "/repo/.git"),
            Git_CommonDirFromFile(SVL("")).Length == 0);

  // A main checkout, one linked worktree by absolute gitdir, one by relative
  // gitdir, a submodule, and a folder outside git.
  StrView main = Fs_Join(a, root, SVL("main"));
  StrView gitDir = Fs_Join(a, main, SVL(".git"));
  CHECK(Write(a, Fs_Join(a, gitDir, SVL("HEAD")), SVL("ref: refs/heads/main\n")));
  CHECK(Fs_MkdirP(a, Fs_Join(a, main, SVL("src/deep"))));

  StrView wtAdmin = Fs_Join(a, gitDir, SVL("worktrees/wt"));
  CHECK(Write(a, Fs_Join(a, wtAdmin, SVL("commondir")), SVL("../..\n")));
  CHECK(Write(a, Fs_Join(a, wtAdmin, SVL("HEAD")), SVL("ref: refs/heads/wt\n")));
  StrBuf gitdirLine = StrBuf_Make(a, 256);
  StrBuf_AppendCStr(&gitdirLine, "gitdir: ");
  StrBuf_Append(&gitdirLine, Fs_ToPosix(a, wtAdmin));
  StrBuf_AppendChar(&gitdirLine, '\n');
  StrView wt = Fs_Join(a, root, SVL("wt"));
  CHECK(Write(a, Fs_Join(a, wt, SVL(".git")), StrBuf_View(&gitdirLine)));

  StrView wt2Admin = Fs_Join(a, gitDir, SVL("worktrees/wt2"));
  CHECK(Write(a, Fs_Join(a, wt2Admin, SVL("commondir")), SVL("../..")));
  StrView wt2 = Fs_Join(a, root, SVL("wt2"));
  CHECK(Write(a, Fs_Join(a, wt2, SVL(".git")),
              SVL("gitdir: ../main/.git/worktrees/wt2\n")));

  StrView sub = Fs_Join(a, main, SVL("sub"));
  CHECK(Fs_MkdirP(a, Fs_Join(a, gitDir, SVL("modules/sub"))));
  CHECK(Write(a, Fs_Join(a, sub, SVL(".git")), SVL("gitdir: ../.git/modules/sub\n")));

  StrView plain = Fs_Join(a, root, SVL("plain"));
  CHECK(Fs_MkdirP(a, plain));

  StrView store = Fs_Join(a, root, SVL("store"));
  WikiLocation fromMain = {};
  WikiLocation fromDeep = {};
  WikiLocation fromWt = {};
  WikiLocation fromWt2 = {};
  WikiLocation fromSub = {};
  WikiLocation fromPlain = {};
  CHECK(Wiki_Locate(a, store, main, &fromMain));
  CHECK(Wiki_Locate(a, store, Fs_Join(a, main, SVL("src/deep")), &fromDeep));
  CHECK(Wiki_Locate(a, store, wt, &fromWt));
  CHECK(Wiki_Locate(a, store, wt2, &fromWt2));
  CHECK(Wiki_Locate(a, store, sub, &fromSub));
  CHECK(Wiki_Locate(a, store, plain, &fromPlain));
  CHECK_ALL(Fs_PathEquals(fromMain.Repository, Fs_RealPath(a, main)),
            StrView_Eq(fromMain.Root, fromDeep.Root),
            StrView_Eq(fromMain.Root, fromWt.Root),
            StrView_Eq(fromMain.Root, fromWt2.Root),
            !StrView_Eq(fromMain.Root, fromSub.Root),
            !StrView_Eq(fromMain.Root, fromPlain.Root),
            StrView_Eq(fromMain.Pages, Fs_Join(a, fromMain.Root, SVL("pages"))),
            // A submodule's files are read from its own work tree.
            Fs_PathEquals(fromSub.Repository, Fs_RealPath(a, sub)));
  // Outside git the folder is its own repository, unless some ancestor of the
  // temp directory happens to be one.
  if (!Sys_GitCommonDir(a, plain).Length)
    CHECK(Fs_PathEquals(fromPlain.Repository, Fs_RealPath(a, plain)));
}

void TestPagesAndLinks(Arena* a) {
  CHECK_ALL(Wiki_ValidSlug(SVL("build-and-test")), Wiki_ValidSlug(SVL("a1")),
            !Wiki_ValidSlug(SVL("")), !Wiki_ValidSlug(SVL("Build")),
            !Wiki_ValidSlug(SVL("-lead")), !Wiki_ValidSlug(SVL("trail-")),
            !Wiki_ValidSlug(SVL("has space")), !Wiki_ValidSlug(SVL("dot.md")));

  Array<StrView> links = Array_Make<StrView>(a, 8);
  Wiki_ParseLinks(a,
                  SVL("See [[alpha]] and `[[inline-code]]`, then [[beta|the beta]].\n"
                      "```\n[[fenced]]\n```\n"
                      "Again [[alpha]] and [[gamma#section]] but not [[unclosed\n"),
                  &links);
  CHECK(links.Count == 3);
  CHECK_ALL(StrView_EqCStr(links.Data[0], "alpha"), StrView_EqCStr(links.Data[1], "beta"),
            StrView_EqCStr(links.Data[2], "gamma"));

  WikiPage page = {};
  Wiki_ParsePage(a, SVL("build"),
                 Page(a, "Build", "project", "Use build.bat", "Body [[test]]\n", true),
                 &page);
  CHECK_ALL(page.HasFrontmatter, StrView_EqCStr(page.Title, "Build"),
            page.Type == WikiPageType::Project, page.Pinned,
            StrView_EqCStr(page.Summary, "Use build.bat"), page.Links.Count == 1,
            StrView_EqCStr(page.Body, "Body [[test]]"));
  WikiPage bare = {};
  Wiki_ParsePage(a, SVL("bare"), SVL("just text, no header"), &bare);
  CHECK_ALL(!bare.HasFrontmatter, bare.Type == WikiPageType::Unknown, !bare.Pinned);
}

void TestLintAndCatalog(Arena* a, StrView root) {
  StrView pages = Fs_Join(a, root, SVL("lint/pages"));
  StrBuf longSummary = StrBuf_Make(a, 200);
  for (u32 i = 0; i < 170; i++) StrBuf_AppendChar(&longSummary, 's');
  CHECK(Write(a, Fs_Join(a, pages, SVL("good.md")),
              Page(a, "Good", "topic", "Fine", "Links [[pinned-one]] and [[missing]]\n")));
  CHECK(Write(a, Fs_Join(a, pages, SVL("pinned-one.md")),
              Page(a, "Pinned", "user", "Always relevant", "Pinned body\n", true)));
  CHECK(Write(a, Fs_Join(a, pages, SVL("wordy.md")),
              Page(a, "Good", "nonsense", StrView_DupCStr(a, StrBuf_View(&longSummary)),
                   "[[good]]\n")));
  CHECK(Write(a, Fs_Join(a, pages, SVL("Bad_Name.md")), SVL("no header [[good]]")));
  CHECK(Write(a, Fs_Join(a, pages, SVL("notes.txt")), SVL("ignored")));

  Wiki wiki = {};
  Wiki_Scan(a, pages, &wiki);
  CHECK_ALL(wiki.Pages.Count == 4, wiki.PinnedPages == 1);
  Array<WikiIssue> issues = Array_Make<WikiIssue>(a, 16);
  Wiki_Lint(a, &wiki, &issues);
  CHECK_ALL(HasIssue(&issues, WikiIssueKind::BrokenLink, "good"),
            HasIssue(&issues, WikiIssueKind::InvalidType, "wordy"),
            HasIssue(&issues, WikiIssueKind::LongSummary, "wordy"),
            HasIssue(&issues, WikiIssueKind::DuplicateTitle, "wordy"),
            HasIssue(&issues, WikiIssueKind::InvalidSlug, "Bad_Name"),
            HasIssue(&issues, WikiIssueKind::MissingFrontmatter, "Bad_Name"),
            !HasIssue(&issues, WikiIssueKind::Orphan, "pinned-one"),
            HasIssue(&issues, WikiIssueKind::Orphan, "Bad_Name"));
  CHECK(Wiki_ErrorCount(&issues) == 5);

  // Pinned pages lead the catalog, and their bodies are included in full.
  StrBuf block = StrBuf_Make(a, 2048);
  WikiCatalogStats stats = {};
  Wiki_AppendPromptBlock(a, &block, &wiki, SVL("C:/store/pages"), true, &stats);
  StrView text = StrBuf_View(&block);
  CHECK_ALL(StrView_Contains(text, "<memory_wiki path=\"C:/store/pages\">"),
            StrView_Contains(text, "When you learn something"),
            StrView_Contains(text, "<page slug=\"pinned-one\">\nPinned body\n</page>"),
            StrView_Contains(text, "<pages>\n- [[pinned-one]] (user) Always relevant\n"),
            StrView_Contains(text, "</pages>\n</memory_wiki>\n"),
            stats.Listed == 4, stats.Omitted == 0);
  StrBuf readOnly = StrBuf_Make(a, 2048);
  Wiki_AppendPromptBlock(a, &readOnly, &wiki, SVL("p"), false, nullptr);
  CHECK(!StrView_Contains(StrBuf_View(&readOnly), "When you learn something"));

  // A catalog past its line budget ends with a count of what it left out.
  Wiki big = {};
  big.Pages = Array_Make<WikiPage>(a, 260);
  for (u32 i = 0; i < 250; i++) {
    WikiPage generated = {};
    StrBuf slug = StrBuf_Make(a, 16);
    StrBuf_AppendCStr(&slug, "page-");
    StrBuf_AppendU64(&slug, i);
    generated.Slug = StrBuf_View(&slug);
    generated.TypeText = SVL("topic");
    generated.Summary = SVL("summary");
    generated.ModifiedMs = i;
    Array_Push(&big.Pages, generated);
  }
  StrBuf catalog = StrBuf_Make(a, 16384);
  WikiCatalogStats bigStats = {};
  Wiki_AppendCatalog(a, &catalog, &big, &bigStats);
  CHECK_ALL(bigStats.Listed == WIKI_CATALOG_MAX_LINES - 1,
            bigStats.Omitted == 250 - bigStats.Listed,
            StrView_Contains(StrBuf_View(&catalog), "- [[page-249]] (topic) summary\n"),
            StrView_Contains(StrBuf_View(&catalog), "more pages are not listed"));
}

void TestPromptBlock(Arena* a, StrView root) {
  StrView store = Fs_Join(a, root, SVL("prompt-store"));
  StrView work = Fs_Join(a, root, SVL("prompt-work"));
  CHECK(Fs_MkdirP(a, work));
  Tool read = {};
  read.Name = SVL("read");
  Tool write = {};
  write.Name = SVL("write");
  Tool tools[] = {read, write};
  PromptContextOptions options = {};
  options.WorkingDirectory = work;
  options.Tools = tools;
  options.ToolCount = 2;
  options.NoContextFiles = true;
  options.NoSkills = true;
  options.NoPromptTemplates = true;
  options.ProjectTrusted = true;

  PromptContext off = {};
  StrBuf error = StrBuf_Make(a, 128);
  CHECK(PromptContext_Build(&options, &off, &error));
  CHECK_ALL(!StrView_Contains(off.SystemPrompt, "memory_wiki"), !off.MemoryPages.Length);

  options.MemoryStore = store;
  PromptContext on = {};
  CHECK(PromptContext_Build(&options, &on, &error));
  i32 blockAt = -1;
  StrView needle = SVL("\n\n<memory_wiki path=\"");
  for (u32 i = 0; i + needle.Length <= on.SystemPrompt.Length && blockAt < 0; i++)
    if (memcmp(on.SystemPrompt.Data + i, needle.Data, needle.Length) == 0) blockAt = (i32)i;
  // The block is spliced in before the cwd line and changes nothing else.
  CHECK(blockAt > 0);
  if (blockAt > 0) {
    StrView before = {on.SystemPrompt.Data, (u32)blockAt};
    CHECK(off.SystemPrompt.Length > before.Length &&
          memcmp(off.SystemPrompt.Data, before.Data, before.Length) == 0);
  }
  CHECK_ALL(StrView_Contains(on.SystemPrompt, "(none yet)"),
            StrView_Contains(on.SystemPrompt, "</memory_wiki>\n\nCurrent working directory: ") ||
                StrView_Contains(on.SystemPrompt, "</memory_wiki>\nCurrent working directory: "),
            on.MemoryPages.Length > 0);
  PromptContext_Release(&on);

  options.Tools = &write;
  options.ToolCount = 1;
  PromptContext withoutRead = {};
  CHECK(PromptContext_Build(&options, &withoutRead, &error));
  CHECK(!StrView_Contains(withoutRead.SystemPrompt, "memory_wiki"));
  PromptContext_Release(&withoutRead);
  PromptContext_Release(&off);
}

ToolArgs Args(Arena* a, StrView path) {
  StrBuf json = StrBuf_Make(a, path.Length + 32);
  StrBuf_AppendCStr(&json, "{\"path\":");
  Json_WriteString(&json, path);
  StrBuf_AppendChar(&json, '}');
  ToolArgs args = {};
  args.Json = StrBuf_View(&json);
  args.Root = Json_ParseCopy(a, args.Json);
  return args;
}

ToolOutcome Ok(Arena* a) {
  ToolOutcome outcome = {};
  outcome.Content = Array_Make<Content>(a, 1);
  Array_Push(&outcome.Content, Content_Text(SVL("written")));
  return outcome;
}

void TestMemoryExtension(Arena* a, StrView root) {
  StrView store = Fs_Join(a, root, SVL("ext-store"));
  StrView work = Fs_Join(a, root, SVL("ext-work"));
  CHECK(Fs_MkdirP(a, work));
  MemoryExtension memory = {};
  MemoryExtension_Init(&memory, nullptr, store, true);
  AppExtension extension = MemoryExtension_Make(&memory);
  AppSessionContext session = {};
  session.SessionId = SVL("session-one");
  session.ProjectKey = SVL("ext-work-key");
  session.WorkingDirectory = work;
  session.TranscriptPath = SVL("t.jsonl");
  extension.SessionChanged(&memory, &session);
  CHECK_ALL(Fs_IsDir(memory.Location.Pages), memory.Pages.Length > 0,
            memory.WritableRoots[0].Length > 0);

  // Only pages are the model's to write.
  AgentToolGateDecision decision = {};
  ToolArgs journalArgs = Args(a, memory.Location.Journal);
  extension.Hooks.BeforeTool(&memory, a, SVL("write"), SVL("c1"), &journalArgs, &decision);
  CHECK(decision.Block);
  decision = {};
  StrView pagePath = Fs_Join(a, memory.Location.Pages, SVL("build.md"));
  ToolArgs pageArgs = Args(a, pagePath);
  extension.Hooks.BeforeTool(&memory, a, SVL("write"), SVL("c2"), &pageArgs, &decision);
  CHECK(!decision.Block);
  ToolArgs workArgs = Args(a, SVL("notes.md"));
  extension.Hooks.BeforeTool(&memory, a, SVL("edit"), SVL("c3"), &workArgs, &decision);
  CHECK(!decision.Block);

  // A page with problems is saved, and the model is told what to fix.
  CHECK(Write(a, pagePath, Page(a, "Build", "project", "", "See [[missing]]\n")));
  ToolOutcome outcome = Ok(a);
  extension.Hooks.AfterTool(&memory, a, SVL("write"), SVL("c2"), &pageArgs, &outcome);
  CHECK(outcome.Content.Count == 2);
  if (outcome.Content.Count == 2)
    CHECK_ALL(StrView_Contains(outcome.Content.Data[1].Text, "build.md was saved but needs fixing"),
              StrView_Contains(outcome.Content.Data[1].Text, "[[missing]]"),
              StrView_Contains(outcome.Content.Data[1].Text, "no summary"));
  CHECK(Write(a, pagePath, Page(a, "Build", "project", "Use build.bat", "Body\n")));
  ToolOutcome clean = Ok(a);
  extension.Hooks.AfterTool(&memory, a, SVL("edit"), SVL("c4"), &pageArgs, &clean);
  CHECK(clean.Content.Count == 1);
  ToolArgs strayArgs = Args(a, Fs_Join(a, memory.Location.Pages, SVL("notes.txt")));
  ToolOutcome stray = Ok(a);
  extension.Hooks.AfterTool(&memory, a, SVL("write"), SVL("c5"), &strayArgs, &stray);
  CHECK(stray.Content.Count == 2);
  ToolOutcome failed = Ok(a);
  failed.IsError = true;
  extension.Hooks.AfterTool(&memory, a, SVL("write"), SVL("c6"), &pageArgs, &failed);
  CHECK(failed.Content.Count == 1);
  CHECK(CountJournal(a, memory.Location.Journal, WikiJournalKind::Page) == 2);

  // The session is journaled once, at its first prompt.
  Message prompt = {};
  CHECK(SessionResult_IsOk(extension.Hooks.BeforeRun(&memory, a, &prompt)));
  CHECK(SessionResult_IsOk(extension.Hooks.BeforeRun(&memory, a, &prompt)));
  CHECK(CountJournal(a, memory.Location.Journal, WikiJournalKind::Session) == 1);

  MemoryExtension_SetEnabled(&memory, false);
  decision = {};
  extension.Hooks.BeforeTool(&memory, a, SVL("write"), SVL("c7"), &journalArgs, &decision);
  CHECK(!decision.Block);
  extension.Shutdown(&memory);
}

SessionRecord MessageRecord(u64 seq, Message message) {
  SessionRecord record = {};
  record.Seq = seq;
  record.Kind = SessionRecordKind::Message;
  record.MessageEntry.Value = message;
  return record;
}

void TestDigest(Arena* a) {
  Message user = Message_User(a, SVL("Please always use build.bat"));
  Message assistant = Message_AssistantEmpty(a);
  Array_Push(&assistant.Content, Content_Text(SVL("Running the build")));
  Array_Push(&assistant.Content,
             Content_ToolCall(SVL("call-1"), SVL("shell"), SVL("{\"command\":\"build.bat\"}")));
  Message thinking = Message_AssistantEmpty(a);
  Array_Push(&thinking.Content, Content_Thinking(SVL("secret reasoning")));
  StrBuf longOutput = StrBuf_Make(a, 4096);
  for (u32 i = 0; i < 3000; i++) StrBuf_AppendChar(&longOutput, 'x');
  Message result = Message_ToolResult(a, SVL("call-1"), SVL("shell"),
                                      StrBuf_View(&longOutput), false);
  Message failure = Message_ToolResult(a, SVL("call-2"), SVL("edit"),
                                       SVL("old text not found"), true);
  SessionRecord records[6] = {};
  records[0].Seq = 1;
  records[0].Kind = SessionRecordKind::SessionStarted;
  records[1] = MessageRecord(2, user);
  records[2] = MessageRecord(3, assistant);
  records[3] = MessageRecord(4, thinking);
  records[4] = MessageRecord(5, result);
  records[5] = MessageRecord(6, failure);

  DreamDigest digest = {};
  Dream_RenderDigest(a, records, 6, 0, SVL("s1"), SVL("C:/work"), DREAM_INBOX_MAX_BYTES,
                     &digest);
  StrView md = digest.Markdown;
  CHECK_ALL(digest.FirstSeq == 2, digest.LastSeq == 6, !digest.Truncated,
            StrView_Contains(md, "# Session s1"), StrView_Contains(md, "Entries: 2-6"),
            StrView_Contains(md, "## User\n\nPlease always use build.bat"),
            StrView_Contains(md, "- tool call `shell`: {\"command\":\"build.bat\"}"),
            StrView_Contains(md, "## Tool result: edit (error)\n\nold text not found"),
            StrView_Contains(md, "more bytes]"),
            !StrView_Contains(md, "secret reasoning"));

  DreamDigest later = {};
  Dream_RenderDigest(a, records, 6, 6, SVL("s1"), SVL("C:/work"), DREAM_INBOX_MAX_BYTES,
                     &later);
  CHECK_ALL(!later.Markdown.Length, later.LastSeq == 6);

  DreamDigest clipped = {};
  Dream_RenderDigest(a, records, 6, 0, SVL("s1"), SVL("C:/work"), 700, &clipped);
  CHECK_ALL(clipped.Truncated, clipped.LastSeq >= 2, clipped.LastSeq < 6,
            clipped.Markdown.Length <= 700);

  WikiLocation location = {SVL("R"), SVL("X"), SVL("P"), SVL("J"), SVL("D")};
  CHECK(StrView_Contains(
      Dream_ExpandPrompt(a, SVL("pages={{pages}} limit={{page_limit}}"), &location),
      "pages=P limit=8 KB"));
}

void TestDreamState(Arena* a, StrView root) {
  StrView path = Fs_Join(a, root, SVL("state/state.json"));
  DreamState missing = {};
  CHECK_ALL(DreamState_Load(a, path, &missing), missing.JournalOffset == 0,
            missing.Sessions.Count == 0);
  DreamState state = {};
  state.JournalOffset = 321;
  state.Sessions = Array_Make<DreamSessionState>(a, 2);
  DreamSessionState session = {SVL("id-1"), SVL("key"), SVL("C:/w"), 42, 1000};
  Array_Push(&state.Sessions, session);
  CHECK(Fs_MkdirP(a, Fs_Parent(path)));
  CHECK(DreamState_Save(a, path, &state));
  DreamState loaded = {};
  CHECK(DreamState_Load(a, path, &loaded));
  CHECK(loaded.Sessions.Count == 1);
  if (loaded.Sessions.Count == 1)
    CHECK_ALL(loaded.JournalOffset == 321, loaded.Sessions.Data[0].Seq == 42,
              loaded.Sessions.Data[0].SeenMs == 1000,
              StrView_EqCStr(loaded.Sessions.Data[0].ProjectKey, "key"));
  CHECK(Write(a, path, SVL("not json")));
  CHECK(!DreamState_Load(a, path, &loaded));
}

void TestDreamExecute(Arena* a, StrView root) {
  StrView work = Fs_Join(a, root, SVL("dream-work"));
  CHECK(Fs_MkdirP(a, work));
  WikiLocation location = {};
  CHECK(Wiki_Locate(a, Fs_Join(a, root, SVL("dream-store")), work, &location));

  // A source session for dreaming to replay, journaled the way the memory
  // extension does it.
  MemoryRepository memory = {};
  MemoryRepository_Init(&memory);
  SessionRepository sessions = MemoryRepository_AsRepository(&memory);
  SessionCreateOptions create = {};
  create.ProjectKey = SVL("dream-work-key");
  create.WorkingDirectory = work;
  SessionLog log = {};
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(sessions.Create(sessions.Ctx, a, &create, &log, &info)));
  ScriptStep sourceSteps[1] = {};
  sourceSteps[0].Text = SVL("Noted: build.bat");
  sourceSteps[0].Stop = StopReason::Stop;
  ScriptProvider sourceScript = {sourceSteps, 1, 0};
  AgentHarnessConfig sourceConfig = {};
  sourceConfig.SystemPrompt = SVL("system");
  sourceConfig.ModelInfo.Id = SVL("script");
  sourceConfig.ModelInfo.Provider = SVL("script");
  sourceConfig.ProviderImpl = ScriptProvider_Make(&sourceScript);
  AgentHarness source = {};
  CHECK(SessionResult_IsOk(AgentHarness_Init(&source, log, &sourceConfig)));
  CHECK(SessionResult_IsOk(AgentHarness_Run(&source, SVL("Always use build.bat"))));
  CHECK(SessionResult_IsOk(AgentHarness_Close(&source)));
  WikiJournalEntry journaled = {};
  journaled.Kind = WikiJournalKind::Session;
  // First prompted long before this first pass; it must still be dreamed.
  journaled.TimeMs = NowMillis() - DREAM_RECHECK_MS - 24ull * 60 * 60 * 1000;
  journaled.SessionId = StrView_Dup(a, DurableId_View(&info.SessionId));
  journaled.ProjectKey = create.ProjectKey;
  journaled.WorkingDirectory = work;
  CHECK(WikiJournal_Append(a, location.Journal, &journaled));

  // The dream writes a page with a broken link and an escape attempt; the
  // escape is blocked and lint earns one follow-up that fixes the link.
  ScriptToolCall firstCalls[] = {
      {SVL("write"), SVL("d1"),
       SVL("{\"path\":\"pages/build.md\",\"content\":\"---\\ntitle: Build\\ntype: "
           "feedback\\nsummary: Always build with build.bat\\npinned: false\\n---\\n"
           "See [[missing]]\\n\"}")},
      {SVL("write"), SVL("d2"), SVL("{\"path\":\"journal.jsonl\",\"content\":\"x\"}")}};
  ScriptToolCall fixCalls[] = {
      {SVL("write"), SVL("d3"),
       SVL("{\"path\":\"pages/build.md\",\"content\":\"---\\ntitle: Build\\ntype: "
           "feedback\\nsummary: Always build with build.bat\\npinned: false\\n---\\n"
           "The user asked for build.bat.\\n\"}")}};
  ScriptStep steps[4] = {};
  steps[0].Tools = firstCalls;
  steps[0].ToolCount = 2;
  steps[1].Text = SVL("Created [[build]].");
  steps[1].Stop = StopReason::Stop;
  steps[2].Tools = fixCalls;
  steps[2].ToolCount = 1;
  steps[3].Text = SVL("Fixed the broken link.");
  steps[3].Stop = StopReason::Stop;
  ScriptProvider script = {steps, 4, 0};

  DreamRun run = {};
  run.Location = location;
  run.Sources = &sessions;
  run.Sessions = &sessions;
  run.ProviderImpl = ScriptProvider_Make(&script);
  run.ModelInfo.Id = SVL("script");
  run.ModelInfo.Provider = SVL("script");
  run.SystemPrompt = SVL("dream");
  run.NowMs = NowMillis();
  StrView cwd = Sys_CurrentDir(a);
  u64 journalBefore = Read(a, location.Journal).Length;

  DreamResult result = {};
  Dream_Execute(a, &run, &result);
  CHECK_ALL(result.Outcome == DreamOutcome::Dreamed, result.Digests == 1,
            result.PagesChanged == 1, script.Index == 4,
            StrView_EqCStr(result.Message, "Fixed the broken link."),
            StrView_Eq(Sys_CurrentDir(a), cwd));
  StrView journal = Read(a, location.Journal);
  CHECK_ALL(StrView_Contains(Read(a, Fs_Join(a, location.Pages, SVL("build.md"))),
                             "The user asked for build.bat."),
            journal.Length > journalBefore,
            CountJournal(a, location.Journal, WikiJournalKind::Dream) == 1,
            // The sandbox refused the dream's write to the journal.
            CountJournal(a, location.Journal, WikiJournalKind::Session) == 1,
            !Fs_Exists(Fs_Join(a, location.DreamDir, SVL("inbox"))),
            Fs_Exists(Fs_Join(a, location.DreamDir, SVL("state.json"))));

  // The progress a watcher reads names the pass's session, whose log replays
  // the dream from its first prompt to its report.
  bool running = true;
  DreamProgress progress = {};
  CHECK(Dream_ReadProgress(a, &location, &running, &progress));
  CHECK_ALL(!running, progress.Phase == DreamPhase::Finished,
            progress.Outcome == DreamOutcome::Dreamed, progress.StartedMs == run.NowMs,
            progress.Digests == 1, progress.PagesChanged == 1,
            StrView_Eq(progress.SessionId, DurableId_View(&result.SessionId)));
  Array<Message> messages = {};
  CHECK(SessionResult_IsOk(Dream_ReadMessages(a, &sessions, &progress, &messages)));
  CHECK(messages.Count > 2);
  if (messages.Count > 2) {
    const Message& first = messages.Data[0];
    const Message& last = messages.Data[messages.Count - 1];
    CHECK_ALL(first.Kind == Role::User, first.Content.Count > 0,
              last.Kind == Role::Assistant, last.Content.Count > 0);
    if (first.Content.Count && last.Content.Count)
      CHECK_ALL(StrView_Contains(first.Content.Data[0].Text, "Dream over"),
                StrView_EqCStr(last.Content.Data[0].Text, "Fixed the broken link."));
  }

  // Nothing new since: no agent run, no new snapshot, and no session to watch.
  DreamResult idle = {};
  Dream_Execute(a, &run, &idle);
  CHECK_ALL(idle.Outcome == DreamOutcome::NothingToDo, script.Index == 4);
  CHECK(Dream_ReadProgress(a, &location, &running, &progress));
  CHECK_ALL(progress.Phase == DreamPhase::Finished,
            progress.Outcome == DreamOutcome::NothingToDo, !progress.SessionId.Length,
            StrView_EqCStr(progress.Message, "Nothing new to dream about."));

  DreamStatus status = {};
  Dream_ReadStatus(a, &location, &status);
  CHECK_ALL(!status.Running, status.HasLastDream, status.Snapshots == 1,
            status.PendingSessions == 0, StrView_EqCStr(status.LastDream.Status, "ok"));

  u32 restored = 99;
  StrView snapshot = {};
#if defined(_WIN32)
  // A second pass is turned away while the lock is held. POSIX record locks
  // belong to the process, so only Windows can show this from one process.
  StrView lockPath = Fs_Join(a, location.DreamDir, SVL("lock"));
  ProcessFileLock lock = {};
  CHECK(ProcessFileLock_TryAcquire(a, lockPath, &lock) == ProcessFileLockStatus::Acquired);
  DreamResult busy = {};
  Dream_Execute(a, &run, &busy);
  CHECK_ALL(busy.Outcome == DreamOutcome::Busy,
            Dream_RestoreLatestSnapshot(a, &location, &restored, &snapshot) ==
                DreamRestoreResult::Busy);
  ProcessFileLock_Release(&lock, lockPath);
#endif

  // The snapshot taken before the pass holds the wiki as it was: empty.
  CHECK(Dream_RestoreLatestSnapshot(a, &location, &restored, &snapshot) ==
        DreamRestoreResult::Restored);
  CHECK_ALL(restored == 0, snapshot.Length > 0,
            !Fs_Exists(Fs_Join(a, location.Pages, SVL("build.md"))));
  MemoryRepository_Destroy(&memory);
}

// A watcher reads the dream's log through its own repository while the pass
// still holds it open for writing, as `/dream watch` does from another process.
void TestDreamMessagesFromLiveLog(Arena* a, StrView root) {
  StrView data = Fs_Join(a, root, SVL("live-sessions"));
  JsonlRepository writerStore = {};
  JsonlRepository_Init(&writerStore, data);
  SessionRepository writer = JsonlRepository_AsRepository(&writerStore);
  SessionCreateOptions create = {};
  create.ProjectKey = SessionProjectKey(a, root);
  create.WorkingDirectory = root;
  create.Label = SVL("dream");
  SessionLog log = {};
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(writer.Create(writer.Ctx, a, &create, &log, &info)));
  ScriptStep steps[1] = {};
  steps[0].Text = SVL("Reading the inbox.");
  steps[0].Stop = StopReason::Stop;
  ScriptProvider script = {steps, 1, 0};
  AgentHarnessConfig config = {};
  config.SystemPrompt = SVL("dream");
  config.ModelInfo.Id = SVL("script");
  config.ModelInfo.Provider = SVL("script");
  config.ProviderImpl = ScriptProvider_Make(&script);
  AgentHarness harness = {};
  CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));

  JsonlRepository readerStore = {};
  JsonlRepository_Init(&readerStore, data);
  SessionRepository reader = JsonlRepository_AsRepository(&readerStore);
  DreamProgress progress = {};
  progress.Phase = DreamPhase::Dreaming;
  progress.SessionId = StrView_Dup(a, DurableId_View(&info.SessionId));
  progress.ProjectKey = create.ProjectKey;
  Array<Message> messages = {};
  // The log is not written until the first prompt.
  CHECK(!SessionResult_IsOk(Dream_ReadMessages(a, &reader, &progress, &messages)));
  CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("Dream over the inbox."))));
  CHECK(SessionResult_IsOk(Dream_ReadMessages(a, &reader, &progress, &messages)));
  CHECK(messages.Count == 2);
  if (messages.Count == 2)
    CHECK_ALL(messages.Data[0].Kind == Role::User,
              messages.Data[1].Kind == Role::Assistant);
  CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
  JsonlRepository_Destroy(&readerStore);
  JsonlRepository_Destroy(&writerStore);
}

void TestDreamWatchState() {
  const u64 since = 1000;
  DreamProgress earlier = {};
  earlier.StartedMs = since - 1;
  earlier.Phase = DreamPhase::Finished;
  DreamProgress preparing = {};
  preparing.StartedMs = since;
  preparing.Phase = DreamPhase::Preparing;
  DreamProgress dreaming = preparing;
  dreaming.Phase = DreamPhase::Dreaming;
  DreamProgress finished = preparing;
  finished.Phase = DreamPhase::Finished;
  const u64 late = since + DREAM_WATCH_START_TIMEOUT_MS;
  CHECK_ALL(
      // The lock is held before the pass writes its own progress.
      Dream_WatchState(true, nullptr, since, since) == DreamWatchState::Starting,
      Dream_WatchState(true, &earlier, since, since) == DreamWatchState::Starting,
      Dream_WatchState(true, &preparing, since, since) == DreamWatchState::Preparing,
      Dream_WatchState(true, &dreaming, since, since) == DreamWatchState::Dreaming,
      Dream_WatchState(true, &finished, since, since) == DreamWatchState::Starting,
      Dream_WatchState(false, &finished, since, since) == DreamWatchState::Finished,
      Dream_WatchState(false, &dreaming, since, since) == DreamWatchState::Stopped,
      // A pass that never reports is given up on.
      Dream_WatchState(false, &earlier, since, late - 1) == DreamWatchState::Starting,
      Dream_WatchState(false, &earlier, since, late) == DreamWatchState::NotStarted,
      // Following whichever pass held the lock accepts any progress.
      Dream_WatchState(false, &earlier, 0, late) == DreamWatchState::Finished);
}

}  // namespace

void TestMemory() {
  Arena arena = ArenaCreate((usize)256 << 20);
  FileLock_Init();
  StrView root = TempRoot(&arena);
  TestRepositoryIdentity(&arena, root);
  TestPagesAndLinks(&arena);
  TestLintAndCatalog(&arena, root);
  TestPromptBlock(&arena, root);
  TestMemoryExtension(&arena, root);
  TestDigest(&arena);
  TestDreamState(&arena, root);
  TestDreamExecute(&arena, root);
  TestDreamMessagesFromLiveLog(&arena, root);
  TestDreamWatchState();
  CHECK(Fs_RemoveTree(&arena, root));
  CHECK(!Fs_Exists(root));
  ArenaRelease(&arena);
}
