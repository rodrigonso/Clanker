#include "app/wiki.h"

#include <string.h>

#include "app/config_paths.h"
#include "app/frontmatter.h"
#include "base/file_io.h"
#include "base/sysinfo.h"
#include "json/json.h"
#include "session/metadata.h"
#include "tools/fs.h"

namespace {

const StrView kTypeNames[] = {SVL(""),        SVL("user"),      SVL("feedback"),
                              SVL("project"), SVL("reference"), SVL("topic")};
constexpr u32 kTypeCount = sizeof(kTypeNames) / sizeof(kTypeNames[0]);

// Pages larger than this are reported by size without being read; they break
// the page budget by orders of magnitude either way.
constexpr u64 kMaxReadBytes = (u64)1 << 20;
constexpr u32 kCatalogSummaryMaxBytes = 240;

constexpr const char* kReadGuidance =
    "This repository keeps a wiki of what earlier sessions learned, shared by "
    "every worktree of it. The catalog below lists each page with its summary, "
    "and pinned pages are included in full. Read a page before relying on it, "
    "and search the folder with grep when the catalog is not enough. A page can "
    "be wrong or out of date, so verify anything that matters before acting on "
    "it.";

constexpr const char* kWriteGuidance =
    "When you learn something a future session would otherwise have to "
    "rediscover or would get wrong, write it into the wiki: the user's "
    "preferences and corrections, decisions and constraints that are not "
    "visible in the code, where to find outside information, and non-obvious "
    "facts about the codebase. Skip what the code, git history, or project "
    "instructions already say, and anything that only matters to the current "
    "task. Keep one subject per page, update an existing page instead of adding "
    "a near-duplicate, link related pages with [[slug]], and prefer edit over "
    "write for pages that exist.\n\n"
    "Each page is <slug>.md in this folder, named in lowercase-kebab-case, and "
    "starts with:\n"
    "---\n"
    "title: Short title\n"
    "type: user | feedback | project | reference | topic\n"
    "summary: One line under 160 characters; it is the page's catalog entry\n"
    "pinned: false\n"
    "---";

WikiPageType ParseType(StrView text) {
  for (u32 i = 1; i < kTypeCount; i++)
    if (StrView_Eq(text, kTypeNames[i])) return (WikiPageType)i;
  return WikiPageType::Unknown;
}

bool HasHeader(StrView content) {
  if (content.Length < 4 || memcmp(content.Data, "---", 3) != 0) return false;
  if (content.Data[3] != '\n' && content.Data[3] != '\r') return false;
  StrView rest = {content.Data + 3, content.Length - 3};
  return StrView_Contains(rest, "\n---");
}

void AppendOneLine(StrBuf* out, StrView text) {
  for (u32 i = 0; i < text.Length; i++) {
    char c = text.Data[i];
    StrBuf_AppendChar(out, c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
  }
}

void AddIssue(Array<WikiIssue>* out, WikiIssueKind kind, StrView slug,
              StrView message, bool error) {
  WikiIssue issue = {};
  issue.Kind = kind;
  issue.Slug = slug;
  issue.Message = message;
  issue.Error = error;
  Array_Push(out, issue);
}

StrView SizeMessage(Arena* arena, const char* prefix, u64 value,
                    const char* suffix) {
  StrBuf message = StrBuf_Make(arena, 96);
  StrBuf_AppendCStr(&message, prefix);
  StrBuf_AppendU64(&message, value);
  StrBuf_AppendCStr(&message, suffix);
  return StrBuf_View(&message);
}

StrView LinkMessage(Arena* arena, const char* prefix, StrView slug,
                    const char* suffix) {
  StrBuf message = StrBuf_Make(arena, slug.Length + 64);
  StrBuf_AppendCStr(&message, prefix);
  StrBuf_Append(&message, slug);
  StrBuf_AppendCStr(&message, suffix);
  return StrBuf_View(&message);
}

// Page indices with pinned pages first, then the most recently modified, then
// by slug so the order is stable across scans.
Array<u32> CatalogOrder(Arena* arena, const Wiki* wiki) {
  u32 count = wiki->Pages.Count;
  Array<u32> order = Array_Make<u32>(arena, count ? count : 1);
  for (u32 i = 0; i < count; i++) Array_Push(&order, i);
  const WikiPage* pages = wiki->Pages.Data;
  Array_Sort(&order, [pages](u32 x, u32 y) {
    const WikiPage* a = &pages[x];
    const WikiPage* b = &pages[y];
    if (a->Pinned != b->Pinned) return a->Pinned;
    if (a->ModifiedMs != b->ModifiedMs) return a->ModifiedMs > b->ModifiedMs;
    return StrView_Compare(a->Slug, b->Slug) < 0;
  });
  return order;
}

}  // namespace

StrView Wiki_DefaultStore(Arena* arena) { return ClankerDataPath(arena, "memory"); }

bool Wiki_Locate(Arena* arena, StrView store, StrView workingDirectory,
                 WikiLocation* out) {
  *out = {};
  if (!arena || !store.Length || !workingDirectory.Length) return false;
  StrView workTree = {};
  StrView common = Sys_GitCommonDir(arena, workingDirectory, &workTree);
  // The key follows the common git directory, which every linked worktree
  // shares. The files to read live in the main checkout when there is one;
  // a submodule or a bare repository's worktree only has the tree it is in.
  StrView identity = {};
  if (common.Length) {
    identity = Fs_RealPath(arena, common);
    bool checkout = Fs_PathEquals(Fs_Base(identity), SVL(".git"));
    if (checkout) identity = StrView_Dup(arena, Fs_Parent(identity));
    out->Repository = checkout ? identity : Fs_RealPath(arena, workTree);
  } else {
    identity = Fs_RealPath(arena, workingDirectory);
    out->Repository = identity;
  }
  out->Root = Fs_Join(arena, store, SessionProjectKey(arena, identity));
  out->Pages = Fs_Join(arena, out->Root, SVL("pages"));
  out->Journal = Fs_Join(arena, out->Root, SVL("journal.jsonl"));
  out->DreamDir = Fs_Join(arena, out->Root, SVL(".dream"));
  return true;
}

bool Wiki_ValidSlug(StrView slug) {
  if (!slug.Length || slug.Length > WIKI_SLUG_MAX_BYTES) return false;
  if (slug.Data[0] == '-' || slug.Data[slug.Length - 1] == '-') return false;
  for (u32 i = 0; i < slug.Length; i++) {
    char c = slug.Data[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

void Wiki_ParseLinks(Arena* arena, StrView body, Array<StrView>* out) {
  bool fenced = false;
  bool inSpan = false;
  u32 i = 0;
  while (i < body.Length) {
    bool lineStart = i == 0 || body.Data[i - 1] == '\n';
    if (lineStart) {
      u32 j = i;
      while (j < body.Length && (body.Data[j] == ' ' || body.Data[j] == '\t')) j++;
      if (j + 3 <= body.Length && (memcmp(body.Data + j, "```", 3) == 0 ||
                                   memcmp(body.Data + j, "~~~", 3) == 0)) {
        fenced = !fenced;
        while (i < body.Length && body.Data[i] != '\n') i++;
        continue;
      }
      inSpan = false;
    }
    char c = body.Data[i];
    if (fenced) {
      i++;
      continue;
    }
    if (c == '`') {
      inSpan = !inSpan;
      i++;
      continue;
    }
    if (inSpan || c != '[' || i + 1 >= body.Length || body.Data[i + 1] != '[') {
      i++;
      continue;
    }
    u32 start = i + 2;
    u32 end = start;
    while (end + 1 < body.Length && body.Data[end] != '\n' &&
           !(body.Data[end] == ']' && body.Data[end + 1] == ']'))
      end++;
    if (end + 1 >= body.Length || body.Data[end] != ']') {
      i = start;
      continue;
    }
    StrView target = {body.Data + start, end - start};
    for (u32 k = 0; k < target.Length; k++) {
      if (target.Data[k] == '|' || target.Data[k] == '#') {
        target.Length = k;
        break;
      }
    }
    target = StrView_Trim(target);
    bool seen = false;
    for (u32 k = 0; k < out->Count && !seen; k++)
      seen = StrView_Eq(out->Data[k], target);
    if (target.Length && !seen) Array_Push(out, StrView_Dup(arena, target));
    i = end + 2;
  }
}

void Wiki_ParsePage(Arena* arena, StrView slug, StrView content, WikiPage* out) {
  *out = {};
  out->Slug = slug;
  out->HasFrontmatter = HasHeader(content);
  FrontmatterField fields[] = {
      {SVL("title"), {}}, {SVL("type"), {}}, {SVL("summary"), {}}, {SVL("pinned"), {}}};
  StrView body = {};
  Frontmatter_Parse(arena, content, fields, sizeof(fields) / sizeof(fields[0]), &body);
  if (out->HasFrontmatter) {
    out->Title = StrView_Trim(fields[0].Value);
    out->TypeText = StrView_Trim(fields[1].Value);
    out->Type = ParseType(out->TypeText);
    out->Summary = StrView_Trim(fields[2].Value);
    out->Pinned = fields[3].Value.Length && Frontmatter_ParseBool(fields[3].Value);
  }
  out->Body = body;
  out->Bytes = content.Length;
  out->Links = Array_Make<StrView>(arena, 8);
  Wiki_ParseLinks(arena, body, &out->Links);
}

void Wiki_Scan(Arena* arena, StrView pagesDir, Wiki* out) {
  *out = {};
  out->Pages = Array_Make<WikiPage>(arena, 16);
  bool listed = false;
  Array<FsEntry> entries = Fs_ListDir(arena, pagesDir, &listed);
  if (!listed) return;
  for (u32 i = 0; i < entries.Count && out->Pages.Count < WIKI_MAX_PAGES; i++) {
    const FsEntry& entry = entries.Data[i];
    if (entry.IsDir || entry.Name.Length <= 3 || entry.Name.Data[0] == '.' ||
        !StrView_EndsWith(entry.Name, SVL(".md")))
      continue;
    StrView slug = {entry.Name.Data, entry.Name.Length - 3};
    StrView path = Fs_Join(arena, pagesDir, entry.Name);
    FsStat stat = Fs_Stat(path);
    StrView content = {};
    if (stat.Size <= kMaxReadBytes &&
        File_ReadAll(arena, path, &content) != FileIoResult::Success)
      continue;
    WikiPage page = {};
    Wiki_ParsePage(arena, StrView_Dup(arena, slug), content, &page);
    page.Bytes = stat.Size;
    page.ModifiedMs = stat.ModifiedMs;
    out->TotalBytes += page.Bytes;
    if (page.Pinned) {
      out->PinnedPages++;
      out->PinnedBytes += page.Body.Length;
    }
    Array_Push(&out->Pages, page);
  }
}

const WikiPage* Wiki_Find(const Wiki* wiki, StrView slug) {
  for (u32 i = 0; wiki && i < wiki->Pages.Count; i++)
    if (StrView_Eq(wiki->Pages.Data[i].Slug, slug)) return &wiki->Pages.Data[i];
  return nullptr;
}

void Wiki_LintPage(Arena* arena, const Wiki* wiki, const WikiPage* page,
                   Array<WikiIssue>* out) {
  if (!Wiki_ValidSlug(page->Slug))
    AddIssue(out, WikiIssueKind::InvalidSlug, page->Slug,
             SVL("the file name must be a lowercase-kebab-case slug of at most 64 "
                 "characters, ending in .md"),
             true);
  if (!page->HasFrontmatter) {
    AddIssue(out, WikiIssueKind::MissingFrontmatter, page->Slug,
             SVL("has no --- header with title, type, summary, and pinned"), true);
  } else {
    if (!page->Title.Length)
      AddIssue(out, WikiIssueKind::MissingTitle, page->Slug,
               SVL("the header has no title"), true);
    if (page->Type == WikiPageType::Unknown)
      AddIssue(out, WikiIssueKind::InvalidType, page->Slug,
               SVL("type must be one of user, feedback, project, reference, or topic"),
               true);
    if (!page->Summary.Length)
      AddIssue(out, WikiIssueKind::MissingSummary, page->Slug,
               SVL("the header has no summary"), true);
    else if (page->Summary.Length > WIKI_SUMMARY_MAX_BYTES)
      AddIssue(out, WikiIssueKind::LongSummary, page->Slug,
               SizeMessage(arena, "the summary is ", page->Summary.Length,
                           " bytes; keep it under 160"),
               true);
  }
  if (page->Bytes > WIKI_PAGE_MAX_BYTES)
    AddIssue(out, WikiIssueKind::PageTooLarge, page->Slug,
             SizeMessage(arena, "the page is ", page->Bytes,
                         " bytes; keep pages under 8 KB by splitting or tightening"),
             true);
  for (u32 i = 0; i < page->Links.Count; i++)
    if (!Wiki_Find(wiki, page->Links.Data[i]))
      AddIssue(out, WikiIssueKind::BrokenLink, page->Slug,
               LinkMessage(arena, "links to [[", page->Links.Data[i],
                           "]], which does not exist"),
               true);
}

void Wiki_Lint(Arena* arena, const Wiki* wiki, Array<WikiIssue>* out) {
  for (u32 i = 0; i < wiki->Pages.Count; i++)
    Wiki_LintPage(arena, wiki, &wiki->Pages.Data[i], out);
  if (wiki->TotalBytes > WIKI_TOTAL_MAX_BYTES)
    AddIssue(out, WikiIssueKind::WikiTooLarge, StrView{},
             SizeMessage(arena, "the wiki is ", wiki->TotalBytes,
                         " bytes; keep it under 200 KB by merging and pruning"),
             true);
  if (wiki->PinnedBytes > WIKI_PINNED_MAX_BYTES)
    AddIssue(out, WikiIssueKind::PinnedTooLarge, StrView{},
             SizeMessage(arena, "pinned pages total ", wiki->PinnedBytes,
                         " bytes; keep them under 4096"),
             true);

  for (u32 i = 0; wiki->Pages.Count > 1 && i < wiki->Pages.Count; i++) {
    const WikiPage* page = &wiki->Pages.Data[i];
    bool linked = page->Pinned;
    for (u32 j = 0; j < wiki->Pages.Count && !linked; j++) {
      if (i == j) continue;
      const WikiPage* other = &wiki->Pages.Data[j];
      for (u32 k = 0; k < other->Links.Count && !linked; k++)
        linked = StrView_Eq(other->Links.Data[k], page->Slug);
    }
    if (!linked)
      AddIssue(out, WikiIssueKind::Orphan, page->Slug,
               SVL("no other page links here"), false);
  }
  for (u32 i = 0; i < wiki->Pages.Count; i++) {
    const WikiPage* page = &wiki->Pages.Data[i];
    for (u32 j = 0; j < i && page->Title.Length; j++) {
      const WikiPage* earlier = &wiki->Pages.Data[j];
      if (!StrView_EqInsensitiveAscii(page->Title, earlier->Title)) continue;
      AddIssue(out, WikiIssueKind::DuplicateTitle, page->Slug,
               LinkMessage(arena, "shares its title with [[", earlier->Slug,
                           "]]; merge them if they cover one subject"),
               false);
      break;
    }
  }
}

u32 Wiki_ErrorCount(const Array<WikiIssue>* issues) {
  u32 count = 0;
  for (u32 i = 0; issues && i < issues->Count; i++)
    if (issues->Data[i].Error) count++;
  return count;
}

void Wiki_AppendIssues(StrBuf* out, const Array<WikiIssue>* issues) {
  for (u32 i = 0; issues && i < issues->Count; i++) {
    const WikiIssue& issue = issues->Data[i];
    StrBuf_AppendCStr(out, issue.Error ? "- error: " : "- warning: ");
    if (issue.Slug.Length) {
      StrBuf_Append(out, issue.Slug);
      StrBuf_AppendCStr(out, ".md: ");
    }
    StrBuf_Append(out, issue.Message);
    StrBuf_AppendChar(out, '\n');
  }
}

void Wiki_AppendCatalog(Arena* arena, StrBuf* out, const Wiki* wiki,
                        WikiCatalogStats* stats) {
  WikiCatalogStats local = {};
  if (!stats) stats = &local;
  Array<u32> order = CatalogOrder(arena, wiki);
  u32 start = out->Length;
  // Room for the closing count line, so a full catalog still ends with it.
  const u32 kReserve = 96;
  StrBuf line = StrBuf_Make(arena, 512);
  for (u32 i = 0; i < order.Count; i++) {
    const WikiPage* page = &wiki->Pages.Data[order.Data[i]];
    StrBuf_Clear(&line);
    StrBuf_AppendCStr(&line, "- [[");
    StrBuf_Append(&line, page->Slug);
    StrBuf_AppendCStr(&line, "]] (");
    StrBuf_Append(&line, page->TypeText.Length ? page->TypeText : SVL("untyped"));
    StrBuf_AppendCStr(&line, ") ");
    StrView summary = page->Summary.Length ? page->Summary
                      : page->Title.Length ? page->Title
                                           : SVL("(no summary)");
    AppendOneLine(&line, StrView_ClampUtf8(summary, kCatalogSummaryMaxBytes));
    StrBuf_AppendChar(&line, '\n');
    if (stats->Listed + 1 >= WIKI_CATALOG_MAX_LINES ||
        out->Length - start + line.Length + kReserve > WIKI_CATALOG_MAX_BYTES)
      break;
    StrBuf_Append(out, StrBuf_View(&line));
    stats->Listed++;
  }
  stats->Omitted = order.Count - stats->Listed;
  if (stats->Omitted) {
    StrBuf_AppendCStr(out, "- ");
    StrBuf_AppendU64(out, stats->Omitted);
    StrBuf_AppendCStr(out, stats->Omitted == 1
                               ? " more page is not listed; search the folder with grep.\n"
                               : " more pages are not listed; search the folder with grep.\n");
  }
}

void Wiki_AppendPromptBlock(Arena* arena, StrBuf* out, const Wiki* wiki,
                            StrView pagesDir, bool writable,
                            WikiCatalogStats* stats) {
  StrBuf_AppendCStr(out, "\n\n<memory_wiki path=\"");
  StrBuf_AppendXmlEscaped(out, pagesDir);
  StrBuf_AppendCStr(out, "\">\n\n");
  StrBuf_AppendCStr(out, kReadGuidance);
  if (writable) {
    StrBuf_AppendCStr(out, "\n\n");
    StrBuf_AppendCStr(out, kWriteGuidance);
  }
  StrBuf_AppendChar(out, '\n');

  Array<u32> order = CatalogOrder(arena, wiki);
  u64 pinnedBytes = 0;
  bool pinnedOpen = false;
  for (u32 i = 0; i < order.Count; i++) {
    const WikiPage* page = &wiki->Pages.Data[order.Data[i]];
    if (!page->Pinned) break;
    StrView body = StrView_Trim(page->Body);
    if (pinnedBytes + body.Length > WIKI_PINNED_MAX_BYTES) continue;
    if (!pinnedOpen) {
      StrBuf_AppendCStr(out, "\n<pinned_pages>\n");
      pinnedOpen = true;
    }
    StrBuf_AppendCStr(out, "<page slug=\"");
    StrBuf_AppendXmlEscaped(out, page->Slug);
    StrBuf_AppendCStr(out, "\">\n");
    StrBuf_Append(out, body);
    StrBuf_AppendCStr(out, "\n</page>\n");
    pinnedBytes += body.Length;
  }
  if (pinnedOpen) StrBuf_AppendCStr(out, "</pinned_pages>\n");

  StrBuf_AppendCStr(out, "\n<pages>\n");
  if (wiki->Pages.Count)
    Wiki_AppendCatalog(arena, out, wiki, stats);
  else
    StrBuf_AppendCStr(out, "(none yet)\n");
  StrBuf_AppendCStr(out, "</pages>\n</memory_wiki>\n");
}

namespace {

const StrView kJournalKinds[] = {SVL(""), SVL("session"), SVL("page"), SVL("dream")};

}  // namespace

bool WikiJournal_Append(Arena* scratch, StrView journalPath,
                        const WikiJournalEntry* entry) {
  if (!scratch || !journalPath.Length || !entry ||
      entry->Kind == WikiJournalKind::Unknown)
    return false;
  usize mark = scratch->Used;
  StrBuf line = StrBuf_Make(scratch, 256);
  JsonWriter w = {};
  JsonWrite_Begin(&w, &line);
  JsonWrite_String(&w, "kind", kJournalKinds[(u32)entry->Kind]);
  JsonWrite_U64(&w, "ts", entry->TimeMs);
  if (entry->SessionId.Length) JsonWrite_String(&w, "session", entry->SessionId);
  if (entry->ProjectKey.Length) JsonWrite_String(&w, "projectKey", entry->ProjectKey);
  if (entry->WorkingDirectory.Length)
    JsonWrite_String(&w, "cwd", entry->WorkingDirectory);
  if (entry->TranscriptPath.Length)
    JsonWrite_String(&w, "transcript", entry->TranscriptPath);
  if (entry->Slug.Length) JsonWrite_String(&w, "slug", entry->Slug);
  if (entry->ToolCallId.Length) JsonWrite_String(&w, "toolCallId", entry->ToolCallId);
  if (entry->Status.Length) JsonWrite_String(&w, "status", entry->Status);
  if (entry->Kind == WikiJournalKind::Dream)
    JsonWrite_U64(&w, "pagesChanged", entry->PagesChanged);
  JsonWrite_End(&w);
  StrBuf_AppendChar(&line, '\n');
  bool ok = Fs_MkdirP(scratch, Fs_Parent(journalPath)) &&
            File_Append(scratch, journalPath, StrBuf_View(&line)) ==
                FileIoResult::Success;
  ArenaRestore(scratch, mark);
  return ok;
}

void WikiJournal_Read(Arena* arena, StrView journalPath, u64 offset,
                      Array<WikiJournalEntry>* out, u64* outEnd) {
  *outEnd = offset;
  StrView content = {};
  if (File_ReadAll(arena, journalPath, &content) != FileIoResult::Success) return;
  // A journal shorter than the offset was replaced; start over rather than
  // skip whatever the new one holds.
  u64 pos = offset <= content.Length ? offset : 0;
  while (pos < content.Length) {
    u64 end = pos;
    while (end < content.Length && content.Data[end] != '\n') end++;
    if (end >= content.Length) break;
    StrView line = StrView_Trim(StrView{content.Data + pos, (u32)(end - pos)});
    pos = end + 1;
    *outEnd = pos;
    JsonValue* root = line.Length ? Json_ParseCopy(arena, line) : nullptr;
    if (!Json_IsObject(root)) continue;
    WikiJournalEntry entry = {};
    StrView kind = Json_GetStr(root, "kind");
    for (u32 i = 1; i < sizeof(kJournalKinds) / sizeof(kJournalKinds[0]); i++)
      if (StrView_Eq(kind, kJournalKinds[i])) entry.Kind = (WikiJournalKind)i;
    if (entry.Kind == WikiJournalKind::Unknown) continue;
    i64 ts = Json_GetInt(root, "ts", 0);
    entry.TimeMs = ts > 0 ? (u64)ts : 0;
    entry.SessionId = Json_GetStr(root, "session");
    entry.ProjectKey = Json_GetStr(root, "projectKey");
    entry.WorkingDirectory = Json_GetStr(root, "cwd");
    entry.TranscriptPath = Json_GetStr(root, "transcript");
    entry.Slug = Json_GetStr(root, "slug");
    entry.ToolCallId = Json_GetStr(root, "toolCallId");
    entry.Status = Json_GetStr(root, "status");
    i64 changed = Json_GetInt(root, "pagesChanged", 0);
    entry.PagesChanged = changed > 0 ? (u32)changed : 0;
    Array_Push(out, entry);
  }
}
