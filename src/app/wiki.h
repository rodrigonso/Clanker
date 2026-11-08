#pragma once

#include "base/arena.h"
#include "base/array.h"
#include "base/str.h"

// The memory wiki: one folder of linked markdown pages per repository, shared
// by every worktree and subdirectory of it, written by the agent during
// sessions and corrected by dreaming. Pages are flat files named
// `<slug>.md` with a scalar frontmatter header; `[[slug]]` links them.
//
//   <store>/<repo-key>/
//     pages/<slug>.md   the wiki
//     journal.jsonl     provenance written by Clanker, never by the model
//     .dream/           dreaming state, inbox, snapshots, and lock
//
// Nothing indexes the pages on disk: the catalog every session sees is
// generated from their headers, so it can never drift from the pages or be
// clobbered by two sessions editing one shared index.

constexpr u32 WIKI_SUMMARY_MAX_BYTES = 160;
constexpr u32 WIKI_SLUG_MAX_BYTES = 64;
constexpr u64 WIKI_PAGE_MAX_BYTES = 8 * 1024;
constexpr u64 WIKI_TOTAL_MAX_BYTES = 200 * 1024;
constexpr u32 WIKI_PINNED_MAX_BYTES = 4 * 1024;
constexpr u32 WIKI_CATALOG_MAX_LINES = 200;
constexpr u32 WIKI_CATALOG_MAX_BYTES = 25 * 1024;
// Scan cap. Far above any useful wiki; it only bounds a runaway folder.
constexpr u32 WIKI_MAX_PAGES = 1000;

enum class WikiPageType : u8 { Unknown, User, Feedback, Project, Reference, Topic };

struct WikiLocation {
  // Where the repository's files can be read: the main worktree's root, or
  // for a submodule or a bare repository the work tree the wiki was located
  // from, or the working directory outside git. The wiki itself is keyed by
  // the git common directory, so it is shared all the same.
  StrView Repository;
  StrView Root;
  StrView Pages;
  StrView Journal;
  StrView DreamDir;
};

// `<home>/.clanker/memory`.
StrView Wiki_DefaultStore(Arena* arena);

// Resolves the wiki for `workingDirectory` under `store`. Every linked worktree
// and subdirectory of one repository resolves to the same location. Creates
// nothing. False only for empty arguments.
bool Wiki_Locate(Arena* arena, StrView store, StrView workingDirectory,
                 WikiLocation* out);

struct WikiPage {
  StrView Slug;
  StrView Title;
  StrView Summary;
  StrView TypeText;
  WikiPageType Type;
  bool HasFrontmatter;
  bool Pinned;
  // Content after the header.
  StrView Body;
  u64 Bytes;
  u64 ModifiedMs;
  Array<StrView> Links;
};

struct Wiki {
  Array<WikiPage> Pages;
  u64 TotalBytes;
  u32 PinnedPages;
  // Body bytes of pinned pages, the share WIKI_PINNED_MAX_BYTES limits.
  u64 PinnedBytes;
};

bool Wiki_ValidSlug(StrView slug);

// Parses one page. `Bytes` is the content's length; `ModifiedMs` is left to
// the caller.
void Wiki_ParsePage(Arena* arena, StrView slug, StrView content, WikiPage* out);

// Distinct `[[slug]]` targets outside code spans and fenced blocks, in
// first-seen order. A `[[slug|label]]` link targets `slug`.
void Wiki_ParseLinks(Arena* arena, StrView body, Array<StrView>* out);

// Reads every `*.md` page directly under `pagesDir`. A missing folder is an
// empty wiki.
void Wiki_Scan(Arena* arena, StrView pagesDir, Wiki* out);

const WikiPage* Wiki_Find(const Wiki* wiki, StrView slug);

enum class WikiIssueKind : u8 {
  InvalidSlug,
  MissingFrontmatter,
  MissingTitle,
  InvalidType,
  MissingSummary,
  LongSummary,
  BrokenLink,
  PageTooLarge,
  WikiTooLarge,
  PinnedTooLarge,
  Orphan,
  DuplicateTitle,
};

struct WikiIssue {
  WikiIssueKind Kind;
  // Empty for wiki-wide issues.
  StrView Slug;
  StrView Message;
  // Errors break the wiki's contract (format, links, budgets); warnings are
  // advice.
  bool Error;
};

// One page's own errors, including links to pages that do not exist.
void Wiki_LintPage(Arena* arena, const Wiki* wiki, const WikiPage* page,
                   Array<WikiIssue>* out);
// Every page's errors plus the wiki-wide budgets, orphans, and duplicates.
void Wiki_Lint(Arena* arena, const Wiki* wiki, Array<WikiIssue>* out);
u32 Wiki_ErrorCount(const Array<WikiIssue>* issues);
// One "- <severity>: <slug>: <message>" line per issue.
void Wiki_AppendIssues(StrBuf* out, const Array<WikiIssue>* issues);

struct WikiCatalogStats {
  u32 Listed;
  u32 Omitted;
};

// "- [[slug]] (type) summary" lines, pinned pages first and then the most
// recently modified, within WIKI_CATALOG_MAX_LINES and WIKI_CATALOG_MAX_BYTES;
// a final line counts what did not fit.
void Wiki_AppendCatalog(Arena* arena, StrBuf* out, const Wiki* wiki,
                        WikiCatalogStats* stats);

// The system prompt's `<memory_wiki>` block: what the wiki is for, the bodies
// of pinned pages within WIKI_PINNED_MAX_BYTES, and the catalog. `writable`
// adds the guidance for writing pages. `pagesDir` is shown as given.
void Wiki_AppendPromptBlock(Arena* arena, StrBuf* out, const Wiki* wiki,
                            StrView pagesDir, bool writable,
                            WikiCatalogStats* stats);

// ---- journal --------------------------------------------------------------
// One JSON object per line, appended by Clanker: which sessions worked in the
// repository (dreaming reads their transcripts, whichever worktree they ran
// in), which pages they wrote, and what each dream did.

enum class WikiJournalKind : u8 { Unknown, Session, Page, Dream };

struct WikiJournalEntry {
  WikiJournalKind Kind;
  u64 TimeMs;
  // Session and Page.
  StrView SessionId;
  // Session.
  StrView ProjectKey;
  StrView WorkingDirectory;
  StrView TranscriptPath;
  // Page.
  StrView Slug;
  StrView ToolCallId;
  // Dream: "ok" or "failed", and the dream's own session.
  StrView Status;
  u32 PagesChanged;
};

bool WikiJournal_Append(Arena* scratch, StrView journalPath,
                        const WikiJournalEntry* entry);

// Complete entries starting at byte `offset`. `*outEnd` is the offset just past
// the last complete line, so a line still being appended is read next time.
void WikiJournal_Read(Arena* arena, StrView journalPath, u64 offset,
                      Array<WikiJournalEntry>* out, u64* outEnd);
