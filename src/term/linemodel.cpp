#include "term/linemodel.h"

#include <string.h>

#include "term/markdown.h"

namespace {

bool g_lmColor = true;
Style LStyle(u8 fg, u8 attr) { return Style{g_lmColor ? fg : COL_DEFAULT, COL_DEFAULT, attr}; }

void NewRow(Arena* a, SpanLine* cur) { cur->Spans = Array_Make<Span>(a, 4); }

}  // namespace

void WrapSpanLine(Arena* a, const SpanLine* in, u16 width, Array<SpanLine>* out) {
  if (width < 1) width = 1;
  SpanLine cur;
  NewRow(a, &cur);
  u16 col = 0;

  for (u32 s = 0; s < in->Spans.Count; s++) {
    Span sp = in->Spans.Data[s];
    StrView t = sp.Text;
    u32 i = 0;
    while (i < t.Length) {
      bool isSpace = (t.Data[i] == ' ');
      u32 start = i;
      while (i < t.Length && (t.Data[i] == ' ') == isSpace) i++;
      StrView tok = StrView{t.Data + start, i - start};

      if (isSpace) {
        if (col == 0) continue;  // drop leading spaces on wrapped rows
        if (col >= width) {
          Array_Push(out, cur);
          NewRow(a, &cur);
          col = 0;
          continue;
        }
        u32 room = (u32)width - col;
        u32 take = tok.Length < room ? tok.Length : room;
        Array_Push(&cur.Spans, Span{StrView{tok.Data, take}, sp.St});
        col = (u16)(col + take);
        continue;
      }

      u32 wW = TextWidth(tok);
      if (col > 0 && (u32)col + wW > width) {
        Array_Push(out, cur);
        NewRow(a, &cur);
        col = 0;
      }
      if ((u32)col + wW <= width) {
        Array_Push(&cur.Spans, Span{tok, sp.St});
        col = (u16)(col + wW);
      } else {
        // Hard-split a word wider than the line; always make progress.
        u32 j = 0;
        while (j < tok.Length) {
          u32 segStart = j;
          u16 segW = 0;
          while (j < tok.Length) {
            u32 jj = j;
            u32 cp = Utf8Decode(tok, &jj);
            u32 cw = CharWidth(cp);
            if (cw < 1) cw = 1;
            bool fresh = (col == 0 && segW == 0);
            if (!fresh && (u32)col + segW + cw > width) break;
            segW = (u16)(segW + cw);
            j = jj;
            if ((u32)col + segW >= width) break;
          }
          Array_Push(&cur.Spans, Span{StrView{tok.Data + segStart, j - segStart}, sp.St});
          col = (u16)(col + segW);
          if (j < tok.Length) {
            Array_Push(out, cur);
            NewRow(a, &cur);
            col = 0;
          }
        }
      }
    }
  }
  Array_Push(out, cur);  // final (possibly empty) row
}

void LineModel_SetColorEnabled(bool on) {
  g_lmColor = on;
  Markdown_SetColorEnabled(on);
}

void LineModel_Init(LineModel* m, Arena* mem, u16 width, u32 cap) {
  m->Mem = mem;
  m->Entries = Array_Make<Entry>(mem, 16);
  m->Lines = Array_Make<SpanLine>(mem, 128);
  m->Width = width ? width : 80;
  m->Cap = cap ? cap : 5000;
  m->CurStart = 0;
  m->HasCur = false;
}

u32 LineModel_Count(const LineModel* m) { return m->Lines.Count; }
const SpanLine* LineModel_At(const LineModel* m, u32 i) {
  return i < m->Lines.Count ? &m->Lines.Data[i] : nullptr;
}

static void WrapAndAppend(LineModel* m, SpanLine* ll) {
  WrapSpanLine(m->Mem, ll, m->Width, &m->Lines);
}

static void AppendPlain(LineModel* m, StrView text, Style st, StrView prefix) {
  if (text.Length == 0) return;
  u32 i = 0;
  while (i <= text.Length) {
    u32 start = i;
    while (i < text.Length && text.Data[i] != '\n') i++;
    StrView ln = StrView{text.Data + start, i - start};
    bool atEnd = i >= text.Length;
    i++;
    SpanLine sl;
    sl.Spans = Array_Make<Span>(m->Mem, 2);
    if (prefix.Length) Array_Push(&sl.Spans, Span{prefix, st});
    if (ln.Length) Array_Push(&sl.Spans, Span{ln, st});
    WrapAndAppend(m, &sl);
    if (atEnd) break;
  }
}

static void LabelRow(LineModel* m, StrView glyph, StrView text, Style st) {
  SpanLine sl;
  sl.Spans = Array_Make<Span>(m->Mem, 2);
  Array_Push(&sl.Spans, Span{glyph, st});
  Array_Push(&sl.Spans, Span{text, st});
  WrapAndAppend(m, &sl);
}

static void BuildEntryRows(LineModel* m, u32 idx) {
  if (idx > 0) {
    SpanLine sep;
    sep.Spans = Array_Make<Span>(m->Mem, 1);
    Array_Push(&m->Lines, sep);  // blank separator row
  }
  Entry* e = &m->Entries.Data[idx];
  StrView text = StrBuf_View(&e->Text);

  switch (e->Kind) {
    case EntryKind::User:
      LabelRow(m, SVL("\xe2\x9d\xaf "), SVL("you"), LStyle(39, ATTR_BOLD));
      AppendPlain(m, text, StyleDefault(), StrView{nullptr, 0});
      break;
    case EntryKind::Assistant: {
      LabelRow(m, SVL("\xe2\x97\x8f "), SVL("assistant"), LStyle(170, ATTR_BOLD));
      if (text.Length) {
        Array<SpanLine> logical = Array_Make<SpanLine>(m->Mem, 8);
        Markdown_Render(m->Mem, text, &logical);
        for (u32 k = 0; k < logical.Count; k++) WrapAndAppend(m, &logical.Data[k]);
      }
    } break;
    case EntryKind::ToolCall: {
      SpanLine sl;
      sl.Spans = Array_Make<Span>(m->Mem, 4);
      Array_Push(&sl.Spans, Span{SVL("\xe2\x9a\x99 "), LStyle(178, ATTR_NONE)});
      Array_Push(&sl.Spans, Span{e->Label, LStyle(178, ATTR_BOLD)});
      if (text.Length) {
        Array_Push(&sl.Spans, Span{SVL(" "), LStyle(244, ATTR_NONE)});
        Array_Push(&sl.Spans, Span{text, LStyle(244, ATTR_NONE)});
      }
      WrapAndAppend(m, &sl);
    } break;
    case EntryKind::ToolResult:
      AppendPlain(m, text, LStyle(244, ATTR_DIM), SVL("  "));
      break;
    case EntryKind::Info:
      AppendPlain(m, text, LStyle(244, ATTR_NONE), StrView{nullptr, 0});
      break;
    case EntryKind::Error:
      AppendPlain(m, text, LStyle(196, ATTR_BOLD), StrView{nullptr, 0});
      break;
  }
}

static void RebuildAll(LineModel* m) {
  m->Lines.Count = 0;
  for (u32 i = 0; i < m->Entries.Count; i++) {
    if (i == m->Entries.Count - 1 && m->HasCur) m->CurStart = m->Lines.Count;
    BuildEntryRows(m, i);
  }
}

static void TrimToCap(LineModel* m) {
  while (m->Lines.Count > m->Cap && m->Entries.Count > 1) {
    memmove(m->Entries.Data, m->Entries.Data + 1, (m->Entries.Count - 1) * sizeof(Entry));
    m->Entries.Count--;
    RebuildAll(m);
  }
}

void LineModel_SetWidth(LineModel* m, u16 width) {
  if (width < 1) width = 1;
  if (width == m->Width) return;
  m->Width = width;
  RebuildAll(m);
}

void LineModel_Begin(LineModel* m, EntryKind kind, StrView label, bool markdown) {
  Entry e;
  e.Kind = kind;
  e.Text = StrBuf_Make(m->Mem, 64);
  e.Label = label.Length ? StrView_Dup(m->Mem, label) : StrView{nullptr, 0};
  e.Markdown = markdown;
  Array_Push(&m->Entries, e);
  m->HasCur = true;
  m->CurStart = m->Lines.Count;
  BuildEntryRows(m, m->Entries.Count - 1);
}

void LineModel_Append(LineModel* m, StrView delta) {
  if (!m->HasCur || m->Entries.Count == 0) return;
  Entry* e = &m->Entries.Data[m->Entries.Count - 1];
  StrBuf_Append(&e->Text, delta);
  m->Lines.Count = m->CurStart;  // re-render just this entry
  BuildEntryRows(m, m->Entries.Count - 1);
}

void LineModel_End(LineModel* m) {
  m->HasCur = false;
  TrimToCap(m);
}

void LineModel_AddSimple(LineModel* m, EntryKind kind, StrView label, StrView text) {
  LineModel_Begin(m, kind, label, false);
  LineModel_Append(m, text);
  LineModel_End(m);
}
