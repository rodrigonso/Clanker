#include "tui/internal.h"

#include <string.h>

#include "base/sysinfo.h"

namespace TuiInternal {

const char* kSpinner[10] = {"\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
                            "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
                            "\xe2\xa0\x87", "\xe2\xa0\x8f"};

Style TuiSt(const Tui* t, u8 fg, u8 attr) {
  return Style{t->Color ? fg : COL_DEFAULT, COL_DEFAULT, attr};
}

// Append a C-string to a bounded buffer.
void Put(char* buf, u32 cap, u32* len, const char* s) {
  while (*s && *len < cap - 1) buf[(*len)++] = *s++;
}
void PutView(char* buf, u32 cap, u32* len, StrView s) {
  for (u32 i = 0; i < s.Length && *len < cap - 1; i++)
    buf[(*len)++] = s.Data[i];
}
u32 PutViewCells(char* buf, u32 cap, u32* len, StrView s,
                 u32 maxCells) {
  u32 offset = 0;
  u32 cells = 0;
  while (offset < s.Length) {
    u32 start = offset;
    u32 cp = Utf8Decode(s, &offset);
    u32 width = CharWidth(cp);
    if (width < 1) width = 1;
    if (cells + width > maxCells || *len + offset - start >= cap)
      break;
    for (u32 i = start; i < offset; i++) buf[(*len)++] = s.Data[i];
    cells += width;
  }
  return cells;
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

// Format a count using best-fit byte-style unit scaling (B/KiB/MiB/GiB,
// 1024-based). Sub-unit values print one decimal place (e.g. "3.4KiB").
void PutScaled(char* buf, u32 cap, u32* len, u32 v) {
  const char* units[4] = {"B", "KiB", "MiB", "GiB"};
  u32 unit = 0;
  u64 whole = v;
  u64 scaled10 = whole * 10;  // one decimal digit while unit == 0 (bytes)
  while (whole >= 1024 && unit < 3) {
    scaled10 = (scaled10 + 512) / 1024;  // carry a rounded decimal digit through the shift
    whole /= 1024;
    unit++;
  }
  if (unit == 0) {
    PutNum(buf, cap, len, (u32)whole);
  } else {
    u32 intPart = (u32)(scaled10 / 10);
    u32 frac = (u32)(scaled10 % 10);
    PutNum(buf, cap, len, intPart);
    Put(buf, cap, len, ".");
    char d[2] = {(char)('0' + frac), 0};
    Put(buf, cap, len, d);
  }
  Put(buf, cap, len, units[unit]);
}

// ---- queue ----

void FormatStatus(Tui* t, char* left, u32 leftCap, u32* ll, char* right,
                  u32 rightCap, u32* rl, u16 cols) {
  *ll = 0;
  if (t->CopyNoticeActive) {
    Put(left, leftCap, ll, "copied ");
    PutNum(left, leftCap, ll, t->CopyNoticeChars);
    Put(left, leftCap, ll, t->CopyNoticeChars == 1 ? " char" : " chars");
  } else if (t->OperationActive) {
    Put(left, leftCap, ll, kSpinner[t->Spinner % 10]);
    Put(left, leftCap, ll, " ");
    for (u32 i = 0; i < t->Sess.OperationLabel.Length && *ll < leftCap - 1; i++)
      left[(*ll)++] = t->Sess.OperationLabel.Data[i];
    if (t->Sess.OperationStatus.Length) {
      Put(left, leftCap, ll, ": ");
      for (u32 i = 0; i < t->Sess.OperationStatus.Length && *ll < leftCap - 1; i++)
        left[(*ll)++] = t->Sess.OperationStatus.Data[i];
    }
  } else if (t->State == TuiState::Idle) {
    Put(left, leftCap, ll, "ready");
  } else {
    Put(left, leftCap, ll, kSpinner[t->Spinner % 10]);
    Put(left, leftCap, ll, " ");
    if (t->State == TuiState::Tool) {
      Put(left, leftCap, ll, "running ");
      for (u32 i = 0; i < t->Sess.StateTool.Length && *ll < leftCap - 1; i++)
        left[(*ll)++] = t->Sess.StateTool.Data[i];
    } else if (t->State == TuiState::Retry) {
      Put(left, leftCap, ll, "retrying ");
      for (u32 i = 0; i < t->Sess.RetryStatus.Length && *ll < leftCap - 1; i++)
        left[(*ll)++] = t->Sess.RetryStatus.Data[i];
    } else if (t->State == TuiState::Streaming) {
      Put(left, leftCap, ll, "responding");
    } else {
      Put(left, leftCap, ll, "thinking");
    }
  }

  *rl = 0;
  u32 rightLimit = cols;
  if (rightLimit > rightCap - 1) rightLimit = rightCap - 1;
  u32 rightCells =
      PutViewCells(right, rightCap, rl, t->ModelName, rightLimit);

  char usage[64] = {};
  u32 usageLength = 0;
  if (t->Use.Input || t->Use.Output) {
    Put(usage, 64, &usageLength, "  \xe2\x86\x91");
    PutScaled(usage, 64, &usageLength, t->Use.Input);
    Put(usage, 64, &usageLength, " \xe2\x86\x93");
    PutScaled(usage, 64, &usageLength, t->Use.Output);
  }

  char effort[24] = {};
  u32 effortLength = 0;
  if (t->Profile.Effort != ModelEffort::None) {
    Put(effort, 24, &effortLength, "  e:");
    PutView(effort, 24, &effortLength,
            ModelEffort_Name(t->Profile.Effort));
  }
  u32 usageCells = TextWidth(StrView{usage, usageLength});
  u32 effortCells = TextWidth(StrView{effort, effortLength});
  if (rightCells + effortCells + usageCells <= rightLimit) {
    PutView(right, rightCap, rl, StrView{effort, effortLength});
    rightCells += effortCells;
  }

  char context[24] = {};
  u32 contextLength = 0;
  if (t->Profile.ContextWindow) {
    Put(context, 24, &contextLength, "  ctx:");
    contextLength += ModelContextWindow_Format(
        context + contextLength, 24 - contextLength,
        t->Profile.ContextWindow);
  }
  u32 contextCells = TextWidth(StrView{context, contextLength});
  if (rightCells + contextCells + usageCells <= rightLimit) {
    PutView(right, rightCap, rl, StrView{context, contextLength});
    rightCells += contextCells;
  }

  if (rightCells + usageCells <= rightLimit) {
    PutView(right, rightCap, rl, StrView{usage, usageLength});
  }
}

void BuildStatusComponents(Tui* t, u16 statusY, u16 cols) {
  char* left = (char*)Ui_FrameAlloc(&t->Ui, 256, alignof(char));
  char* right = (char*)Ui_FrameAlloc(&t->Ui, 256, alignof(char));
  u32 leftLength = 0;
  u32 rightLength = 0;
  // The row is inset on both sides, so the right-aligned group clears the
  // transcript scrollbar column. Format against the usable width, not `cols`.
  constexpr u16 kStatusInset = 1;
  u16 contentCols = cols > 2 * kStatusInset ? (u16)(cols - 2 * kStatusInset) : 0;
  FormatStatus(t, left, 256, &leftLength, right, 256, &rightLength, contentCols);

  UiLayoutStyle row = Positioned(0, statusY, cols, 1);
  row.Padding.Left = kStatusInset;
  row.Padding.Right = kStatusInset;
  Ui_BeginRow(&t->Ui, Ui_Key(SVL("status")), row);
  Style style = TuiSt(t, 244, ATTR_NONE);
  UiTextProps leftProps = {};
  leftProps.Layout.Width = Ui_Fill();
  leftProps.Layout.Height = Ui_Fill();
  leftProps.StyleValue =
      t->CopyNoticeActive ? TuiSt(t, COPY_NOTICE_FG, ATTR_NONE) : style;
  Ui_Text(&t->Ui, Ui_Key(SVL("left")), StrView{left, leftLength}, leftProps);
  UiTextProps rightProps = {};
  rightProps.Layout.Width = Ui_Content();
  rightProps.Layout.Height = Ui_Fill();
  rightProps.StyleValue = style;
  Ui_Text(&t->Ui, Ui_Key(SVL("right")), StrView{right, rightLength},
          rightProps);
  Ui_EndContainer(&t->Ui);
}

void BuildCwdComponent(Tui* t, u16 cwdY, u16 cols) {
  if (t->Cwd.Length == 0 || cols <= 1) return;
  Style style = TuiSt(t, 244, ATTR_NONE);
  UiTextSpan spans[4];
  u32 count = 0;
  spans[count++] = UiTextSpan{t->Cwd, style};
  if (t->Branch.Length) {
    spans[count++] = UiTextSpan{SVL("  (\xe2\x8e\x87 "), style};
    spans[count++] = UiTextSpan{t->Branch, style};
    spans[count++] = UiTextSpan{SVL(")"), style};
  }
  UiTextProps props = {};
  props.Layout = Positioned(1, cwdY, (u16)(cols - 1), 1);
  props.StyleValue = style;
  Ui_StyledText(&t->Ui, Ui_Key(SVL("cwd")), spans, count, props);
}

}  // namespace TuiInternal
