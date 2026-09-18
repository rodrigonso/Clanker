#pragma once
#include "base/array.h"
#include "base/str.h"
#include "term/cell.h"
#include "tui/toolview.h"

// A styled run of text and a logical/wrapped line made of such runs.
struct Span {
  StrView Text;
  Style St;
  bool KeepLeading;  // structural indentation; ordinary wrap-leading spaces are dropped
};

struct SpanLine {
  Array<Span> Spans;
  Span Right;
  Span Continuation;
  Span Hidden;
  u64 EntryId;
  bool HasRight;
  bool HasContinuation;
  bool HasHiddenContent;
  bool IsEntryStart;
};

// Resolved visual geometry for a SpanLine at a particular terminal width.
// Each decoded scalar occupies at least one cell, matching Screen_PutText.
// ContentEnd excludes unused blank viewport cells, while explicit extraction
// ranges retain every selected blank cell.
struct DisplayRowLayout {
  const SpanLine* Line;
  u16 Columns;
  u16 LeftLimit;
  u16 HiddenColumn;
  u16 RightColumn;
  u16 ContentEnd;
};

struct DisplayRowRun {
  StrView Text;
  Style St;
  u16 Column;
  u16 CellWidth;
};

// Zero-initialize before the first LineModel_NextDisplayRowRun call.
struct DisplayRowIterator {
  u32 SpanIndex;
  u16 Column;
};

// Half-open cell-column range. Endpoints are clipped, order-normalized, and
// snapped outwards when they land inside a width-two glyph.
struct DisplayRowRange {
  u16 Start;
  u16 End;
};

// Transcript entry kinds (drive layout + base styling).
enum class EntryKind : u8 { User, Assistant, Tool, Info, Error };

struct TextEntry {
  StrBuf Content;
  StrView Timestamp;  // User only
};

struct ToolEntry {
  StrView Id;
  ToolView View;
  StrBuf Preview;
  bool Done;
  bool Error;
};

struct Entry {
  u64 Id;
  EntryKind Kind;
  bool Collapsed;
  bool Dirty;
  bool PreviewShown;
  bool Separator;
  u32 PreviewRows;
  u32 PreviousVisible;
  u64 RowStart;  // uncropped coordinate, including this entry's separator
  Array<SpanLine> Rows;
  usize RowsMark;
  usize RowsBytes;
  union {
    TextEntry Text;
    ToolEntry Tool;
  };
};

// The scrollback: logical entries plus their wrapped display rows for the
// current width. Mutations only invalidate entry caches; display queries
// materialize them together. Both writes and queries require the controller's
// lock, including const queries (which may update the derived layout).
struct LineModel {
  Arena* Mem;
  Arena RowsMem;
  Arena LayoutMem;
  Array<Entry> Entries;
  Array<SpanLine> Lines;  // visible rows, in a ring starting at LinesHead
  u32 LinesHead;
  u32 FirstDirtyEntry;  // UINT32_MAX when clean; mutations lower this directly
  u32 LayoutEntries;
  u32 LastVisibleEntry;
  u64 RowsBegin;  // uncropped coordinates are not renumbered during eviction
  u64 RowsEnd;
  u64 RowOffset;
  u64 ExpandedEntryId;
  u16 Width;
  u32 Cap;                 // soft cap on retained rows (oldest dropped past this)
  u32 AssistantStart;      // first display row of the streaming assistant entry
  u32 AssistantRowOffset;  // rendered assistant rows currently cropped from the front
  usize LiveRowsBytes;
  u64 NextEntryId;
  u64 DisplayRevision;
  bool PreviewBudgetDirty;
  bool TrimPending;
  bool HasAssistant;
  bool Color;
  bool Rounded;
};

void LineModel_Init(LineModel* m, Arena* mem, u16 width, u32 cap,
                    bool color = true, bool rounded = true);
void LineModel_Release(LineModel* m);
void LineModel_Layout(LineModel* m);
void LineModel_SetWidth(LineModel* m, u16 width);  // re-wrap everything
bool LineModel_SetRounded(LineModel* m, bool rounded);
u32 LineModel_Count(const LineModel* m);
// Check whether another width fits within `maxRows` without changing the
// current rendered rows.
bool LineModel_FitsAtWidth(LineModel* m, u16 width, u32 maxRows);
const SpanLine* LineModel_At(const LineModel* m, u32 i);
u64 LineModel_RowEntryId(const LineModel* m, u32 row);
u64 LineModel_DisplayRevision(const LineModel* m);
bool LineModel_FindEntryRows(const LineModel* m, u64 entryId, u32* firstRow, u32* rowCount);
bool LineModel_IsEntryCollapsed(const LineModel* m, u64 entryId);
bool LineModel_ToggleEntry(LineModel* m, u64 entryId);
bool LineModel_ToggleAllTools(LineModel* m);

DisplayRowLayout LineModel_LayoutDisplayRow(const SpanLine* line, u16 columns);
bool LineModel_NextDisplayRowRun(const DisplayRowLayout* layout, DisplayRowIterator* iterator,
                                 DisplayRowRun* run);
DisplayRowRange LineModel_NormalizeDisplayRowRange(const DisplayRowLayout* layout, u16 anchor,
                                                   u16 focus);
StrView LineModel_ExtractDisplayRow(Arena* arena, const DisplayRowLayout* layout, u16 anchor,
                                    u16 focus);
u64 LineModel_HashDisplayRow(const DisplayRowLayout* layout, u16 anchor, u16 focus);

// Appends accumulate without layout or transcript copies. Display queries
// render the dirty assistant once, even if several deltas arrived this turn.
void LineModel_BeginAssistant(LineModel* m);
void LineModel_AppendAssistant(LineModel* m, StrView delta);
void LineModel_EndAssistant(LineModel* m);
void LineModel_AddNotice(LineModel* m, EntryKind kind, StrView text);  // Info or Error
void LineModel_AddUser(LineModel* m, StrView text, u64 timestampMs);
void LineModel_AddTool(LineModel* m, StrView toolId, StrView name, StrView argsJson);
bool LineModel_UpdateTool(LineModel* m, StrView toolId, StrView preview);
bool LineModel_FinishTool(LineModel* m, StrView toolId, StrView result, StrView preview, bool isError);
