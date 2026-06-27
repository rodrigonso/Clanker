#pragma once
#include "base/array.h"
#include "base/str.h"
#include "term/cell.h"

// A styled run of text and a logical/wrapped line made of such runs.
struct Span {
  StrView Text;
  Style St;
};

struct SpanLine {
  Array<Span> Spans;
};

// Transcript entry kinds (drive labels + base styling).
enum class EntryKind : u8 { User, Assistant, ToolCall, ToolResult, Info, Error };

struct Entry {
  EntryKind Kind;
  StrBuf Text;     // accumulated raw text (markdown for Assistant)
  StrView Label;   // tool name / extra label
  bool Markdown;
};

// The scrollback: logical entries plus their wrapped display rows for the
// current width. The agent thread mutates this (under the controller's lock);
// the render thread reads display rows.
struct LineModel {
  Arena* Mem;
  Array<Entry> Entries;
  Array<SpanLine> Lines;  // wrapped rows; each SpanLine is one screen row
  u16 Width;
  u32 Cap;        // soft cap on retained rows (oldest dropped past this)
  u32 CurStart;   // index in Lines where the current (last) entry's rows begin
  bool HasCur;
};

void LineModel_Init(LineModel* m, Arena* mem, u16 width, u32 cap);
void LineModel_SetWidth(LineModel* m, u16 width);  // re-wrap everything
void LineModel_SetColorEnabled(bool on);
u32 LineModel_Count(const LineModel* m);
const SpanLine* LineModel_At(const LineModel* m, u32 i);

// Streaming build API: Begin a new entry, Append deltas (re-renders the current
// entry), End to finalize. AddSimple is begin+append+end for one-shot entries.
void LineModel_Begin(LineModel* m, EntryKind kind, StrView label, bool markdown);
void LineModel_Append(LineModel* m, StrView delta);
void LineModel_End(LineModel* m);
void LineModel_AddSimple(LineModel* m, EntryKind kind, StrView label, StrView text);

// Wrap one logical line to `width`, appending the resulting rows to `out`.
void WrapSpanLine(Arena* a, const SpanLine* in, u16 width, Array<SpanLine>* out);
