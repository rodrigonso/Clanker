#include "tui/perf.h"

#include <string.h>

#include "base/sysinfo.h"
#include "tui/internal.h"

namespace {

u32 SaturatingElapsed(u64 startUs, u64 nowUs) {
  u64 elapsed = nowUs > startUs ? nowUs - startUs : 0;
  return elapsed > 0xffffffffull ? 0xffffffffu : (u32)elapsed;
}

void RecordFrameInterval(TuiPerf* p, u32 intervalUs) {
  if (!intervalUs) return;
  p->Window[p->Head] = intervalUs;
  p->Head = (u8)((p->Head + 1) % TUI_PERF_WINDOW);
  if (p->Count < TUI_PERF_WINDOW) p->Count++;
}

struct PerfSummary {
  u32 IntervalAvgUs;
  u32 IntervalMaxUs;
  u32 FpsTenths;
};

PerfSummary Summarize(const TuiPerf* p) {
  PerfSummary out = {};
  u64 sum = 0;
  for (u32 i = 0; i < p->Count; i++) {
    u32 interval = p->Window[i];
    sum += interval;
    if (interval > out.IntervalMaxUs) out.IntervalMaxUs = interval;
  }
  if (!p->Count) return out;
  out.IntervalAvgUs = (u32)(sum / p->Count);
  out.FpsTenths = (u32)(10000000ull / out.IntervalAvgUs);
  return out;
}

}  // namespace

void TuiPerf_FrameRendered(TuiPerf* p, u64 nowUs) {
  if (p->LastFrameUs)
    RecordFrameInterval(p, SaturatingElapsed(p->LastFrameUs, nowUs));
  p->LastFrameUs = nowUs;
}

void TuiPerf_TurnStarted(TuiPerf* p, u64 nowUs) {
  p->TurnStartUs = nowUs;
  p->ToolStartUs = 0;
  p->LastTokens = {};
  p->LastTtftUs = 0;
  p->LastTurnUs = 0;
  p->LastToolUs = 0;
  p->LastToolLength = 0;
}

void TuiPerf_AssistantDelta(TuiPerf* p, u64 nowUs) {
  if (!p->TurnStartUs || p->LastTtftUs) return;
  u32 elapsed = SaturatingElapsed(p->TurnStartUs, nowUs);
  p->LastTtftUs = elapsed ? elapsed : 1u;
}

void TuiPerf_TurnMessageEnded(TuiPerf* p, Usage use, u64 nowUs) {
  p->LastTokens = {use.Input, use.Output, use.CacheRead};
  if (!p->TurnStartUs) return;
  p->LastTurnUs = SaturatingElapsed(p->TurnStartUs, nowUs);
  p->TurnStartUs = 0;
}

void TuiPerf_ToolStarted(TuiPerf* p, StrView name, u64 nowUs) {
  p->ToolStartUs = nowUs;
  u32 n = ClankMin(name.Length, (u32)sizeof(p->LastTool));
  if (n) memcpy(p->LastTool, name.Data, n);
  p->LastToolLength = (u8)n;
  p->LastToolUs = 0;
}

void TuiPerf_ToolEnded(TuiPerf* p, u64 nowUs) {
  if (!p->ToolStartUs) return;
  p->LastToolUs = SaturatingElapsed(p->ToolStartUs, nowUs);
  p->ToolStartUs = 0;
}

namespace TuiInternal {

namespace {

enum {
  kOverlayInnerWidth = 30,
  kOverlayLineCap = 9,
  kOverlayLineBytes = kOverlayInnerWidth + 1,
  kRssSampleMs = 1000,
};

struct DebugOverlayComponentData {
  UiRect Panel;
  Style PanelStyle;
  Style ValueStyle;
  char Lines[kOverlayLineCap][kOverlayLineBytes];
  u8 LineLengths[kOverlayLineCap];
  u8 LineCount;
};

struct OverlayBuilder {
  DebugOverlayComponentData* Data;
  u32 Length;
};

char* CurrentLine(OverlayBuilder* b) {
  return b->Data->Lines[b->Data->LineCount];
}

void BeginLine(OverlayBuilder* b, const char* label) {
  FFI(b->Data->LineCount >= kOverlayLineCap);
  b->Length = 0;
  Put(CurrentLine(b), kOverlayLineBytes, &b->Length, label);
  while (b->Length < 8) CurrentLine(b)[b->Length++] = ' ';
}

void EndLine(OverlayBuilder* b) {
  b->Data->LineLengths[b->Data->LineCount] = (u8)b->Length;
  b->Data->LineCount++;
}

void Text(OverlayBuilder* b, const char* s) {
  Put(CurrentLine(b), kOverlayLineBytes, &b->Length, s);
}

void Num(OverlayBuilder* b, u32 v) {
  PutNum(CurrentLine(b), kOverlayLineBytes, &b->Length, v);
}

void Scaled(OverlayBuilder* b, u64 v) {
  PutScaled(CurrentLine(b), kOverlayLineBytes, &b->Length, v);
}

void Fixed1(OverlayBuilder* b, u32 tenths) {
  Num(b, tenths / 10);
  Text(b, ".");
  char digit[2] = {(char)('0' + tenths % 10), 0};
  Text(b, digit);
}

void Duration(OverlayBuilder* b, u64 us) {
  if (us < 1000) {
    Num(b, (u32)us);
    Text(b, "us");
    return;
  }
  if (us < 1000000) {
    Fixed1(b, (u32)((us + 50) / 100));
    Text(b, "ms");
    return;
  }
  u64 hundredths = (us + 5000) / 10000;
  if (hundredths >= 6000) {
    Fixed1(b, (u32)((us + 50000) / 100000));
    Text(b, "s");
    return;
  }
  Num(b, (u32)(hundredths / 100));
  Text(b, ".");
  u32 fraction = (u32)(hundredths % 100);
  char digits[3] = {(char)('0' + fraction / 10),
                    (char)('0' + fraction % 10), 0};
  Text(b, digits);
  Text(b, "s");
}

void View(OverlayBuilder* b, StrView s) {
  PutView(CurrentLine(b), kOverlayLineBytes, &b->Length, s);
}

void BuildLines(const TuiPerf* p, OverlayBuilder* b) {
  PerfSummary render = Summarize(p);

  BeginLine(b, "fps");
  if (render.FpsTenths)
    Fixed1(b, render.FpsTenths);
  else
    Text(b, "--");
  EndLine(b);

  BeginLine(b, "frame");
  if (render.IntervalAvgUs) {
    Duration(b, render.IntervalAvgUs);
    Text(b, "  max ");
    Duration(b, render.IntervalMaxUs);
  } else {
    Text(b, "--");
  }
  EndLine(b);

  BeginLine(b, "rss");
  if (p->RssBytes)
    Scaled(b, p->RssBytes);
  else
    Text(b, "--");
  EndLine(b);

  BeginLine(b, "ttft");
  if (p->LastTtftUs)
    Duration(b, p->LastTtftUs);
  else
    Text(b, "--");
  EndLine(b);

  BeginLine(b, "turn");
  if (p->LastTurnUs) {
    Duration(b, p->LastTurnUs);
    if (p->LastTokens.Output) {
      // Turn wall time includes tool execution.
      u64 tenths =
          (u64)p->LastTokens.Output * 10000000ull / p->LastTurnUs;
      Text(b, "  ");
      Fixed1(b, tenths > 0xffffffffull ? 0xffffffffu : (u32)tenths);
      Text(b, " tok/s");
    }
  } else {
    Text(b, "--");
  }
  EndLine(b);

  BeginLine(b, "tokens");
  Num(b, p->LastTokens.Input);
  Text(b, " in  ");
  Num(b, p->LastTokens.Output);
  Text(b, " out");
  EndLine(b);

  if (p->LastTokens.CacheRead) {
    BeginLine(b, "cache");
    Num(b, p->LastTokens.CacheRead);
    Text(b, " r");
    EndLine(b);
  }

  if (p->LastToolLength) {
    BeginLine(b, "tool");
    View(b, StrView{p->LastTool, p->LastToolLength});
    Text(b, " ");
    if (p->ToolStartUs)
      Text(b, "running");
    else
      Duration(b, p->LastToolUs);
    EndLine(b);
  }

  if (p->DroppedEvents) {
    BeginLine(b, "dropped");
    Num(b, p->DroppedEvents);
    EndLine(b);
  }
}

void PaintDebugOverlayNode(UiContext*, const UiNode* node,
                           UiCanvas* canvas) {
  DebugOverlayComponentData* data = (DebugOverlayComponentData*)node->User;
  UiCanvas_Fill(canvas, data->Panel, data->PanelStyle);
  UiCanvas_DrawBorder(canvas, data->Panel, data->PanelStyle);

  u16 x = (u16)(data->Panel.X + 2);
  u16 maxX = (u16)(data->Panel.X + data->Panel.Width - 1);
  Style title = data->PanelStyle;
  title.Attr |= ATTR_BOLD;
  UiCanvas_PutTextClipped(canvas, x, data->Panel.Y, SVL(" debug "), title,
                          maxX);
  for (u32 i = 0; i < data->LineCount; i++)
    UiCanvas_PutTextClipped(canvas, x, (u16)(data->Panel.Y + 1 + i),
                            StrView{data->Lines[i], data->LineLengths[i]},
                            data->ValueStyle, maxX);
}

}  // namespace

void BuildDebugOverlayComponent(Tui* t, u16 cols, u16 rows, u64 nowMs) {
  if (!t->Perf.Visible) return;

  u8 lineCount = 6;
  if (t->Perf.LastTokens.CacheRead) lineCount++;
  if (t->Perf.LastToolLength) lineCount++;
  if (t->Perf.DroppedEvents) lineCount++;
  u16 panelW = (u16)(kOverlayInnerWidth + 4);
  u16 panelH = (u16)(lineCount + 2);
  if (cols < panelW + 2 || rows < panelH + 2) return;

  if (!t->Perf.LastRssSampleMs || nowMs < t->Perf.LastRssSampleMs ||
      nowMs - t->Perf.LastRssSampleMs >= kRssSampleMs) {
    t->Perf.RssBytes = CurrentRssBytes();
    t->Perf.LastRssSampleMs = nowMs;
  }

  DebugOverlayComponentData* data =
      (DebugOverlayComponentData*)Ui_FrameAlloc(
          &t->Ui, sizeof(DebugOverlayComponentData),
          alignof(DebugOverlayComponentData));
  data->Panel = UiRect{(i32)(cols - panelW - 1), 0, panelW, panelH};
  data->PanelStyle = TuiSt(t, 250, ATTR_NONE);
  data->ValueStyle = TuiSt(t, 252, ATTR_NONE);
  data->LineCount = 0;
  OverlayBuilder builder = {data, 0};
  BuildLines(&t->Perf, &builder);
  FFI(data->LineCount != lineCount);

  UiNodeDesc desc = {};
  desc.Kind = UiNodeKind::Custom;
  desc.Key = Ui_Key(SVL("debug-overlay"));
  desc.Ops.Paint = PaintDebugOverlayNode;
  desc.User = data;
  desc.Layout.Width = Ui_Fill();
  desc.Layout.Height = Ui_Fill();
  Ui_Leaf(&t->Ui, desc);
}

}  // namespace TuiInternal
