#pragma once
#include "base/array.h"
#include "base/str.h"
#include "term/cell.h"
#include "term/toolview.h"

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
  u64 EntryId;
  bool HasRight;
  bool HasContinuation;
  bool HasHiddenContent;
  bool IsEntryStart;
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
  union {
    TextEntry Text;
    ToolEntry Tool;
  };
};

// The scrollback: logical entries plus their wrapped display rows for the
// current width. The agent thread mutates this (under the controller's lock);
// the render thread reads display rows.
struct LineModel {
  Arena* Mem;
  Arena RowsMem;
  Array<Entry> Entries;
  Array<SpanLine> Lines;  // wrapped rows; each SpanLine is one screen row
  u16 Width;
  u32 Cap;                 // soft cap on retained rows (oldest dropped past this)
  u32 AssistantStart;      // first display row of the streaming assistant entry
  usize AssistantRowsMark; // RowsMem position before the streaming assistant entry
  u64 NextEntryId;
  bool HasAssistant;
};

void LineModel_Init(LineModel* m, Arena* mem, u16 width, u32 cap);
void LineModel_Release(LineModel* m);
void LineModel_SetWidth(LineModel* m, u16 width);  // re-wrap everything
void LineModel_SetColorEnabled(bool on);
u32 LineModel_Count(const LineModel* m);
const SpanLine* LineModel_At(const LineModel* m, u32 i);
u64 LineModel_RowEntryId(const LineModel* m, u32 row);
bool LineModel_FindEntryRows(const LineModel* m, u64 entryId, u32* firstRow, u32* rowCount);
bool LineModel_IsEntryCollapsed(const LineModel* m, u64 entryId);
bool LineModel_ToggleEntry(LineModel* m, u64 entryId);
bool LineModel_ToggleAllTools(LineModel* m);

// Streaming assistant API. Appends re-render only the active assistant entry.
void LineModel_BeginAssistant(LineModel* m);
void LineModel_AppendAssistant(LineModel* m, StrView delta);
void LineModel_EndAssistant(LineModel* m);
void LineModel_AddNotice(LineModel* m, EntryKind kind, StrView text);  // Info or Error
void LineModel_AddUser(LineModel* m, StrView text, u64 timestampMs);
void LineModel_AddTool(LineModel* m, StrView toolId, StrView name, StrView argsJson);
bool LineModel_FinishTool(LineModel* m, StrView toolId, StrView result, StrView preview, bool isError);

// Wrap one logical line to `width`, appending the resulting rows to `out`.
void WrapSpanLine(Arena* a, const SpanLine* in, u16 width, Array<SpanLine>* out);
