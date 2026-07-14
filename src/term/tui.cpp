#include "term/tui.h"

#include <string.h>

#include "agent/agent.h"
#include "base/sysinfo.h"
#include "term/cell.h"
#include "term/sanitize.h"

namespace {

const char* kSpinner[10] = {"\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
                            "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
                            "\xe2\xa0\x87", "\xe2\xa0\x8f"};

bool g_color = true;
Style St(u8 fg, u8 attr) { return Style{g_color ? fg : COL_DEFAULT, COL_DEFAULT, attr}; }

// Append a C-string to a bounded buffer.
void Put(char* buf, u32 cap, u32* len, const char* s) {
  while (*s && *len < cap - 1) buf[(*len)++] = *s++;
}
void PutNum(char* buf, u32 cap, u32* len, u32 v) {
  char tmp[16];
  u32 n = 0;
  if (v == 0) tmp[n++] = '0';
  while (v) {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n && *len < cap - 1) buf[(*len)++] = tmp[--n];
}

// ---- queue ----
void QPush(Tui* t, StrView line) {
  if (t->QCount >= 64) return;
  t->Queue[t->QTail] = line;
  t->QTail = (t->QTail + 1) % 64;
  t->QCount++;
}
bool QPop(Tui* t, StrView* out) {
  if (t->QCount == 0) return false;
  *out = t->Queue[t->QHead];
  t->QHead = (t->QHead + 1) % 64;
  t->QCount--;
  return true;
}

void Wake(Tui* t) {
  t->Dirty = true;
  Cond_Signal(&t->RenderCv);
}

void FlushPending(Tui* t) {
  if (t->Pending.Length) {
    LineModel_AppendAssistant(&t->Model, StrBuf_View(&t->Pending));
    StrBuf_Clear(&t->Pending);
  }
}

StrView Truncate(StrView s, u32 maxBytes) {
  if (s.Length <= maxBytes) return s;
  u32 n = maxBytes;
  while (n > 0 && ((unsigned char)s.Data[n] & 0xC0) == 0x80) n--;
  return StrView{s.Data, n};
}

// ---- input layout ----
struct InputLayout {
  StrView Rows[256];
  u32 RowCount;
  u32 CurRow;
  u16 CurCol;
};

void LayoutInput(StrView text, u32 cursor, u16 width, InputLayout* out) {
  out->RowCount = 0;
  out->CurRow = 0;
  out->CurCol = 0;
  if (width < 1) width = 1;
  u32 rowStart = 0;
  u16 col = 0;
  u32 i = 0;
  while (true) {
    if (i == cursor) {
      out->CurRow = out->RowCount;
      out->CurCol = col;
    }
    if (i >= text.Length) break;
    if (text.Data[i] == '\n') {
      if (out->RowCount < 256) out->Rows[out->RowCount++] = StrView{text.Data + rowStart, i - rowStart};
      i++;
      rowStart = i;
      col = 0;
      continue;
    }
    u32 jj = i;
    u32 cp = Utf8Decode(text, &jj);
    u32 cw = CharWidth(cp);
    if (cw < 1) cw = 1;
    if ((u32)col + cw > width) {
      if (out->RowCount < 256) out->Rows[out->RowCount++] = StrView{text.Data + rowStart, i - rowStart};
      rowStart = i;
      col = 0;
      if (i == cursor) {
        out->CurRow = out->RowCount;
        out->CurCol = 0;
      }
    }
    col = (u16)(col + cw);
    i = jj;
  }
  if (out->RowCount < 256) out->Rows[out->RowCount++] = StrView{text.Data + rowStart, text.Length - rowStart};
}

Style HoverStyle(Style style) {
  if (g_color)
    style.Bg = 235;
  else
    style.Attr |= ATTR_DIM;
  return style;
}

Style RowTextStyle(Style style, const SpanLine* line, bool hovered) {
  if (hovered) return HoverStyle(style);
  if (line->HasFill) style.Bg = line->Fill.Bg;
  return style;
}

void RenderSpanLine(Screen* s, u16 y, const SpanLine* ln, u16 cols, bool hovered) {
  if (cols && (ln->HasFill || hovered)) {
    Style fill = hovered ? HoverStyle(StyleDefault()) : ln->Fill;
    Screen_Fill(s, 0, y, (u16)(cols - 1), y, fill);
  }

  u16 x = 0;
  u16 leftLimit = cols;
  u32 rightWidth = ln->HasRight ? TextWidth(ln->Right.Text) : 0;
  bool drawRight = ln->HasRight && rightWidth < cols;
  bool drawHidden = ln->HasHiddenContent && cols != 0;
  if (drawHidden && drawRight && rightWidth + 2u >= cols) drawRight = false;
  if (drawRight) leftLimit = (u16)(cols - rightWidth - 1u);

  u16 hiddenX = cols;
  if (drawHidden) {
    hiddenX = drawRight ? (u16)(cols - rightWidth - 2u) : (u16)(cols - 1u);
    if (hiddenX < leftLimit) leftLimit = hiddenX;
  }

  for (u32 i = 0; i < ln->Spans.Count && x < leftLimit; i++) {
    Style style = RowTextStyle(ln->Spans.Data[i].St, ln, hovered);
    x = Screen_PutText(s, x, y, ln->Spans.Data[i].Text, style, leftLimit);
  }
  if (drawHidden) {
    Style style = RowTextStyle(St(244, ATTR_DIM), ln, hovered);
    Screen_PutText(s, hiddenX, y, SVL("\xe2\x80\xa6"), style, (u16)(hiddenX + 1u));
  }
  if (drawRight) {
    Style style = RowTextStyle(ln->Right.St, ln, hovered);
    Screen_PutText(s, (u16)(cols - rightWidth), y, ln->Right.Text, style, cols);
  }
}

void ComposeStatus(Tui* t, u16 statusY, u16 cols) {
  Style bg = St(COL_DEFAULT, ATTR_NONE);
  bg.Bg = g_color ? 236 : COL_DEFAULT;
  Screen_Fill(&t->Scr, 0, statusY, (u16)(cols ? cols - 1 : 0), statusY, bg);

  Style st = bg;
  st.Fg = g_color ? 250 : COL_DEFAULT;

  // Left: agent state.
  char left[256];
  u32 ll = 0;
  if (t->State == TuiState::Idle) {
    Put(left, sizeof(left), &ll, "ready");
  } else {
    Put(left, sizeof(left), &ll, kSpinner[t->Spinner % 10]);
    Put(left, sizeof(left), &ll, " ");
    if (t->State == TuiState::Tool) {
      Put(left, sizeof(left), &ll, "running ");
      for (u32 i = 0; i < t->StateTool.Length && ll < sizeof(left) - 1; i++) left[ll++] = t->StateTool.Data[i];
    } else if (t->State == TuiState::Streaming) {
      Put(left, sizeof(left), &ll, "responding");
    } else {
      Put(left, sizeof(left), &ll, "thinking");
    }
  }

  // Right (right-aligned): model name + token usage.
  char right[256];
  u32 rl = 0;
  for (u32 i = 0; i < t->ModelName.Length && rl < sizeof(right) - 1; i++) right[rl++] = t->ModelName.Data[i];
  if (t->Use.Input || t->Use.Output) {
    Put(right, sizeof(right), &rl, "  \xe2\x86\x91");
    PutNum(right, sizeof(right), &rl, t->Use.Input);
    Put(right, sizeof(right), &rl, " \xe2\x86\x93");
    PutNum(right, sizeof(right), &rl, t->Use.Output);
  }

  StrView rightView{right, rl};
  u32 rightW = TextWidth(rightView);
  u16 rightX = (u32)cols > rightW ? (u16)(cols - rightW) : 0;

  // Draw the state on the left, clipped so it never overruns the right block.
  Screen_PutText(&t->Scr, 0, statusY, StrView{left, ll}, st, rightX ? rightX : cols);
  Screen_PutText(&t->Scr, rightX, statusY, rightView, st, cols);
}

// Sticky working-directory + git-branch line, drawn on the row just above the
// prompt. Muted so it reads as ambient context, not transcript content.
void ComposeCwd(Tui* t, u16 cwdY, u16 cols) {
  if (t->Cwd.Length == 0) return;
  Style st = St(g_color ? 244 : COL_DEFAULT, ATTR_NONE);
  u16 x = Screen_PutText(&t->Scr, 0, cwdY, t->Cwd, st, cols);
  if (t->Branch.Length) {
    x = Screen_PutText(&t->Scr, x, cwdY, SVL("  ("), st, cols);
    x = Screen_PutText(&t->Scr, x, cwdY, t->Branch, st, cols);
    Screen_PutText(&t->Scr, x, cwdY, SVL(")"), st, cols);
  }
}

struct ViewportAnchor {
  u64 EntryId;
  u32 EntryRowOffset;
  u32 ScreenRow;
};

ViewportAnchor CaptureViewportAnchor(const Tui* t) {
  ViewportAnchor anchor = {};
  if (t->Follow) return anchor;

  u32 count = LineModel_Count(&t->Model);
  u32 maxTop = count > t->LastPaneH ? count - t->LastPaneH : 0;
  u32 top = t->Top < maxTop ? t->Top : maxTop;
  for (u32 row = top; row < count; row++) {
    u64 id = LineModel_RowEntryId(&t->Model, row);
    u32 first = 0;
    if (id == 0 || !LineModel_FindEntryRows(&t->Model, id, &first, nullptr)) continue;
    anchor.EntryId = id;
    anchor.EntryRowOffset = row - first;
    anchor.ScreenRow = row - top;
    break;
  }
  return anchor;
}

void RestoreViewportAnchor(Tui* t, ViewportAnchor anchor) {
  if (t->Follow) return;

  u32 first = 0;
  u32 count = 0;
  if (anchor.EntryId != 0 &&
      LineModel_FindEntryRows(&t->Model, anchor.EntryId, &first, &count)) {
    u32 offset = anchor.EntryRowOffset < count ? anchor.EntryRowOffset : count - 1;
    u32 target = first + offset;
    t->Top = target > anchor.ScreenRow ? target - anchor.ScreenRow : 0;
  }

  u32 lineCount = LineModel_Count(&t->Model);
  u32 maxTop = lineCount > t->LastPaneH ? lineCount - t->LastPaneH : 0;
  if (t->Top > maxTop) t->Top = maxTop;
}

void Compose(Tui* t) {
  u16 cols = 0, rows = 0;
  Term_Size(&t->Trm, &cols, &rows);
  if (cols != t->Scr.Cols || rows != t->Scr.Rows) {
    ViewportAnchor anchor = CaptureViewportAnchor(t);
    Screen_Resize(&t->Scr, cols, rows);
    LineModel_SetWidth(&t->Model, cols);
    RestoreViewportAnchor(t, anchor);
  }
  if (cols < 4 || rows < 4) {
    Screen_BeginFrame(&t->Scr);
    return;
  }

  ViewportAnchor pendingAnchor = CaptureViewportAnchor(t);
  FlushPending(t);
  RestoreViewportAnchor(t, pendingAnchor);
  if (t->State != TuiState::Idle) t->Spinner++;

  // Input box layout.
  u16 textW = (u16)(cols > 2 ? cols - 2 : 1);
  InputLayout il;
  LayoutInput(LineEdit_Text(&t->Edit), LineEdit_Cursor(&t->Edit), textW, &il);
  u16 maxInputH = (u16)(rows / 2);
  if (maxInputH < 1) maxInputH = 1;
  if (maxInputH > 10) maxInputH = 10;
  u16 inputH = (u16)(il.RowCount < maxInputH ? il.RowCount : maxInputH);
  if (inputH < 1) inputH = 1;

  // Reserve the bottom row for the status bar and the row just above the prompt
  // for the sticky cwd/branch line; keep at least one transcript row (rows >= 4
  // is guaranteed above).
  u16 statusY = (u16)(rows - 1);
  u16 maxInput = (u16)(rows - 3);
  if (inputH > maxInput) inputH = maxInput;
  if (inputH < 1) inputH = 1;
  u16 inputTop = (u16)(statusY - inputH);
  u16 cwdY = (u16)(inputTop - 1);
  u16 paneH = cwdY;  // rows [0, cwdY)
  t->LastPaneH = paneH;

  Screen_BeginFrame(&t->Scr);

  // Transcript.
  u32 count = LineModel_Count(&t->Model);
  u32 top;
  if (t->Follow) {
    top = count > paneH ? count - paneH : 0;
  } else {
    u32 maxTop = count > paneH ? count - paneH : 0;
    top = t->Top > maxTop ? maxTop : t->Top;
  }
  t->LastTop = top;
  if (t->HasMouse && t->MouseY < paneH) {
    const SpanLine* hoverLine = LineModel_At(&t->Model, top + t->MouseY);
    t->HoveredEntryId = hoverLine && hoverLine->IsEntryStart ? hoverLine->EntryId : 0;
  } else {
    t->HoveredEntryId = 0;
  }

  for (u16 r = 0; r < paneH; r++) {
    u32 idx = top + r;
    if (idx >= count) break;
    const SpanLine* line = LineModel_At(&t->Model, idx);
    bool hovered =
        line->IsEntryStart && line->EntryId != 0 && line->EntryId == t->HoveredEntryId;
    RenderSpanLine(&t->Scr, r, line, cols, hovered);
  }

  // Status.
  ComposeStatus(t, statusY, cols);

  // Input box (scroll within if taller than the box).
  u32 firstRow = il.CurRow >= inputH ? il.CurRow - inputH + 1 : 0;
  for (u16 i = 0; i < inputH; i++) {
    u32 rr = firstRow + i;
    if (rr >= il.RowCount) break;
    StrView prefix = (rr == 0) ? SVL("\xe2\x9d\xaf ") : SVL("  ");
    u16 x = Screen_PutText(&t->Scr, 0, (u16)(inputTop + i), prefix, St(39, ATTR_BOLD), cols);
    Screen_PutText(&t->Scr, x, (u16)(inputTop + i), il.Rows[rr], StyleDefault(), cols);
  }
  u16 curScreenRow = (u16)(inputTop + (il.CurRow - firstRow));
  u16 curScreenCol = (u16)(2 + il.CurCol);
  if (curScreenCol >= cols) curScreenCol = (u16)(cols - 1);
  Screen_SetCursor(&t->Scr, true, curScreenCol, curScreenRow);

  // Sticky context line above the prompt.
  ComposeCwd(t, cwdY, cols);
}

// ---- key handling (lock held) ----
void DoSubmit(Tui* t) {
  StrView line = LineEdit_Submit(&t->Edit);
  if (line.Length == 0) return;
  FlushPending(t);
  if (t->Model.HasAssistant) LineModel_EndAssistant(&t->Model);
  LineModel_AddUser(&t->Model, line, NowMillis());
  t->Follow = true;
  if (t->State != TuiState::Idle && t->Ag) {
    Agent_Steer(t->Ag, line);  // mid-turn submit -> steer the running agent
  } else {
    QPush(t, line);  // idle -> next prompt for Tui_WaitForSubmit
    Cond_Signal(&t->SubmitCv);
  }
}

const i32 kWheelStep = 3;  // transcript rows per mouse-wheel notch

u64 HitTestEntry(const Tui* t, u16 y) {
  if (y >= t->LastPaneH) return 0;
  const SpanLine* line = LineModel_At(&t->Model, t->LastTop + y);
  return line && line->IsEntryStart ? line->EntryId : 0;
}

void UpdateMouse(Tui* t, const Key& key) {
  t->MouseX = key.MouseX;
  t->MouseY = key.MouseY;
  t->HasMouse = true;
  t->HoveredEntryId = HitTestEntry(t, key.MouseY);
}

void ScrollBy(Tui* t, i32 delta) {
  u32 count = LineModel_Count(&t->Model);
  u32 maxTop = count > t->LastPaneH ? count - t->LastPaneH : 0;
  u32 cur = t->Follow ? maxTop : t->Top;
  i64 nv = (i64)cur + delta;
  if (nv < 0) nv = 0;
  if (nv > (i64)maxTop) nv = maxTop;
  t->Top = (u32)nv;
  t->Follow = (t->Top >= maxTop);
}

void HandleKey(Tui* t, const Key& k) {
  switch (k.Kind) {
    case KeyKind::Char:
      if (!k.Alt && k.Codepoint >= 0x20) LineEdit_InsertChar(&t->Edit, k.Codepoint);
      break;
    case KeyKind::Enter: DoSubmit(t); break;
    case KeyKind::Newline: LineEdit_Newline(&t->Edit); break;
    case KeyKind::Backspace: LineEdit_Backspace(&t->Edit); break;
    case KeyKind::Delete: LineEdit_Delete(&t->Edit); break;
    case KeyKind::Left: LineEdit_Left(&t->Edit); break;
    case KeyKind::Right: LineEdit_Right(&t->Edit); break;
    case KeyKind::Up:
      if (LineEdit_OnFirstLine(&t->Edit) || !LineEdit_Up(&t->Edit))
        LineEdit_HistoryPrev(&t->Edit);
      break;
    case KeyKind::Down:
      if (LineEdit_OnLastLine(&t->Edit) || !LineEdit_Down(&t->Edit))
        LineEdit_HistoryNext(&t->Edit);
      break;
    case KeyKind::Home: LineEdit_Home(&t->Edit); break;
    case KeyKind::End: LineEdit_End(&t->Edit); break;
    case KeyKind::WordLeft: LineEdit_WordLeft(&t->Edit); break;
    case KeyKind::WordRight: LineEdit_WordRight(&t->Edit); break;
    case KeyKind::CtrlW: LineEdit_DeleteWordBack(&t->Edit); break;
    case KeyKind::CtrlU: LineEdit_KillToStart(&t->Edit); break;
    case KeyKind::CtrlK: LineEdit_KillToEnd(&t->Edit); break;
    case KeyKind::CtrlL: t->Scr.ForceRepaint = true; break;
    case KeyKind::CtrlO: {
      ViewportAnchor anchor = CaptureViewportAnchor(t);
      if (LineModel_ToggleAllTools(&t->Model)) RestoreViewportAnchor(t, anchor);
      break;
    }
    case KeyKind::ScrollUp: ScrollBy(t, -kWheelStep); break;
    case KeyKind::ScrollDown: ScrollBy(t, kWheelStep); break;
    case KeyKind::MouseMove:
      UpdateMouse(t, k);
      if (t->PressedEntryId != 0 && t->PressedEntryId != t->HoveredEntryId)
        t->PressedEntryId = 0;
      break;
    case KeyKind::MouseLeftDown:
      UpdateMouse(t, k);
      t->PressedEntryId = t->HoveredEntryId;
      break;
    case KeyKind::MouseLeftUp: {
      UpdateMouse(t, k);
      u64 pressed = t->PressedEntryId;
      t->PressedEntryId = 0;
      if (pressed == 0 || pressed != t->HoveredEntryId) break;
      ViewportAnchor anchor = CaptureViewportAnchor(t);
      if (LineModel_ToggleEntry(&t->Model, pressed)) RestoreViewportAnchor(t, anchor);
      break;
    }
    case KeyKind::CtrlC:
      if (t->State != TuiState::Idle)
        t->AbortFlag = 1;
      else if (!LineEdit_Empty(&t->Edit))
        LineEdit_Clear(&t->Edit);
      else {
        t->Quit = true;
        t->Stop = true;
        Cond_Signal(&t->SubmitCv);
      }
      break;
    case KeyKind::CtrlD:
      t->AbortFlag = 1;
      t->Quit = true;
      t->Stop = true;
      Cond_Signal(&t->SubmitCv);
      break;
    default: break;
  }
}

// ---- threads ----
void InputLoop(void* arg) {
  Tui* t = (Tui*)arg;
  char buf[1024];
  for (;;) {
    u32 n = Term_ReadInput(&t->Trm, buf, sizeof(buf));
    if (n == 0) {
      Mutex_Lock(&t->Lock);
      t->Quit = true;
      t->Stop = true;
      Cond_Signal(&t->SubmitCv);
      Wake(t);
      Mutex_Unlock(&t->Lock);
      break;
    }
    Tui_FeedInput(t, buf, n);
    Mutex_Lock(&t->Lock);
    bool stop = t->Stop;
    Mutex_Unlock(&t->Lock);
    if (stop) break;
  }
}

void RenderLoop(void* arg) {
  Tui* t = (Tui*)arg;
  for (;;) {
    Mutex_Lock(&t->Lock);
    if (t->Stop) {
      Mutex_Unlock(&t->Lock);
      break;
    }
    Compose(t);
    Mutex_Unlock(&t->Lock);
    Screen_Flush(&t->Scr);

    Mutex_Lock(&t->Lock);
    u32 interval = (t->State != TuiState::Idle) ? 60 : 200;
    if (!t->Dirty && !t->Stop) Cond_WaitTimeout(&t->RenderCv, &t->Lock, interval);
    t->Dirty = false;
    Mutex_Unlock(&t->Lock);
  }
}

}  // namespace

bool Tui_Start(Tui* t, Arena* mem, TuiOpts opts) {
  if (!Term_IsInteractive()) return false;
  Tui_InitModel(t, mem, opts);
  if (!Term_Enter(&t->Trm)) {
    LineModel_Release(&t->Model);
    return false;
  }

  u16 cols = 80, rows = 24;
  Term_Size(&t->Trm, &cols, &rows);
  Screen_Init(&t->Scr, mem, [](void* u, const char* d, u32 n) { (void)u; Term_Write(d, n); }, nullptr);
  Screen_Resize(&t->Scr, cols, rows);
  LineModel_SetWidth(&t->Model, cols);

  StrView raw = Sys_CurrentDir(mem);
  t->Cwd = Sys_AbbreviatePath(mem, raw, Sys_HomeDir(mem));
  t->Branch = Sys_GitBranch(mem, raw);

  Thread_Start(&t->RenderThread, RenderLoop, t);
  Thread_Start(&t->InputThread, InputLoop, t);
  t->ThreadsUp = true;
  return true;
}

void Tui_InitModel(Tui* t, Arena* mem, TuiOpts opts) {
  memset(t, 0, sizeof(*t));
  t->Mem = mem;
  g_color = opts.Color;
  LineModel_SetColorEnabled(opts.Color);
  t->ModelName = opts.ModelName;
  LineModel_Init(&t->Model, mem, 80, 5000);
  LineEdit_Init(&t->Edit, mem);
  KeyDecoder_Init(&t->Dec);
  t->Pending = StrBuf_Make(mem, 256);
  t->ToolText = StrBuf_Make(mem, 256);
  Mutex_Init(&t->Lock);
  Cond_Init(&t->SubmitCv);
  Cond_Init(&t->RenderCv);
  t->Follow = true;
  t->State = TuiState::Idle;
}

void Tui_Stop(Tui* t) {
  if (t->ThreadsUp) {
    Mutex_Lock(&t->Lock);
    t->Stop = true;
    Cond_Signal(&t->SubmitCv);
    Cond_Signal(&t->RenderCv);
    Mutex_Unlock(&t->Lock);
    Thread_Join(&t->RenderThread);
    Thread_Join(&t->InputThread);
    t->ThreadsUp = false;
    Term_Exit(&t->Trm);
  }
  LineModel_Release(&t->Model);
}

bool Tui_WaitForSubmit(Tui* t, StrView* out) {
  Mutex_Lock(&t->Lock);
  for (;;) {
    if (QPop(t, out)) {
      Mutex_Unlock(&t->Lock);
      return true;
    }
    if (t->Quit || t->Stop) {
      Mutex_Unlock(&t->Lock);
      return false;
    }
    Cond_Wait(&t->SubmitCv, &t->Lock);
  }
}

volatile i32* Tui_Abort(Tui* t) { return &t->AbortFlag; }

void Tui_FeedInput(Tui* t, const char* data, u32 n) {
  KeyDecoder_Push(&t->Dec, data, n);
  Mutex_Lock(&t->Lock);
  Key k;
  while (KeyDecoder_Next(&t->Dec, &k)) HandleKey(t, k);
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_AgentEvent(void* user, const AgentEvent& ev) {
  Tui* t = (Tui*)user;
  Mutex_Lock(&t->Lock);
  ViewportAnchor anchor = CaptureViewportAnchor(t);
  switch (ev.Kind) {
    case AgentEventKind::AgentStart:
      t->State = TuiState::Thinking;
      t->TurnStartMs = NowMillis();
      t->Spinner = 0;
      break;
    case AgentEventKind::MessageStart:
      // The agent emits message_start for the assistant turn before any content
      // exists (ev.Msg is the not-yet-built message, often null), so there is
      // nothing to render yet. The assistant entry is begun lazily on its first
      // text delta below. This mirrors pi, where the assistant block stays empty
      // (invisible) until content streams, so tool-only turns show no header.
      break;
    case AgentEventKind::MessageUpdate:
      if (ev.Stream.Kind == StreamEventKind::TextDelta) {
        if (!t->Model.HasAssistant) LineModel_BeginAssistant(&t->Model);
        StrBuf_Append(&t->Pending, ev.Stream.Delta);
        t->State = TuiState::Streaming;
      }
      break;
    case AgentEventKind::MessageEnd:
      FlushPending(t);
      if (t->Model.HasAssistant) LineModel_EndAssistant(&t->Model);
      if (ev.Msg) {
        t->Use = ev.Msg->Use;
        if (ev.Msg->Stop == StopReason::Error)
          LineModel_AddNotice(&t->Model, EntryKind::Error, ev.Msg->ErrorMessage);
        else if (ev.Msg->Stop == StopReason::Aborted)
          LineModel_AddNotice(&t->Model, EntryKind::Info, SVL("[interrupted]"));
      }
      break;
    case AgentEventKind::ToolStart:
      FlushPending(t);
      if (t->Model.HasAssistant) LineModel_EndAssistant(&t->Model);
      LineModel_AddTool(&t->Model, ev.ToolCallId, ev.ToolName, ev.ToolArgsJson);
      t->State = TuiState::Tool;
      t->StateTool = ev.ToolName;
      break;
    case AgentEventKind::ToolEnd: {
      StrView preview = StrView{nullptr, 0};
      if (ev.Result && ev.Result->Content.Count > 0) preview = ev.Result->Content.Data[0].Text;
      StrBuf_Clear(&t->ToolText);
      Sanitize_Display(&t->ToolText, preview);  // strip \r / ANSI / control bytes from tool output
      StrView sanitized = StrBuf_View(&t->ToolText);
      if (!LineModel_FinishTool(&t->Model, ev.ToolCallId, sanitized, Truncate(sanitized, 1500),
                                ev.IsError)) {
        LineModel_AddNotice(&t->Model, EntryKind::Error,
                            SVL("tool result did not match a pending call"));
      }
      t->State = TuiState::Thinking;
      break;
    }
    case AgentEventKind::AgentEnd:
      FlushPending(t);
      if (t->Model.HasAssistant) LineModel_EndAssistant(&t->Model);
      t->State = TuiState::Idle;
      break;
    case AgentEventKind::QueueUpdate:
      t->QueuedCount = ev.SteerCount + ev.FollowUpCount + ev.NextTurnCount;
      break;
    default: break;
  }
  RestoreViewportAnchor(t, anchor);
  Wake(t);
  Mutex_Unlock(&t->Lock);
}
