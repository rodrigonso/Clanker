#include "term/cell.h"

u32 CharWidth(u32 cp) {
  if (cp == 0) return 1;  // blank cell renders as a space

  // Zero-width: combining marks, joiners, variation selectors, BOM.
  if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
      (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x200B && cp <= 0x200F) ||
      (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF)
    return 0;

  // Wide: CJK + fullwidth + emoji.
  if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) ||
      (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
      (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
      (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
      (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
      (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) ||
      (cp >= 0x20000 && cp <= 0x3FFFD))
    return 2;

  return 1;
}

u32 Utf8Decode(StrView s, u32* i) {
  if (*i >= s.Length) return 0;
  unsigned char c = (unsigned char)s.Data[*i];
  u32 need, cp;
  if (c < 0x80) {
    *i += 1;
    return c;
  } else if ((c >> 5) == 0x6) {
    need = 2;
    cp = c & 0x1f;
  } else if ((c >> 4) == 0xe) {
    need = 3;
    cp = c & 0x0f;
  } else if ((c >> 3) == 0x1e) {
    need = 4;
    cp = c & 0x07;
  } else {
    *i += 1;
    return 0xFFFD;
  }
  if (*i + need > s.Length) {
    *i = s.Length;
    return 0xFFFD;
  }
  for (u32 k = 1; k < need; k++) cp = (cp << 6) | ((unsigned char)s.Data[*i + k] & 0x3f);
  *i += need;
  return cp;
}

u32 TextWidth(StrView s) {
  u32 i = 0, w = 0;
  while (i < s.Length) {
    u32 cp = Utf8Decode(s, &i);
    w += CharWidth(cp);
  }
  return w;
}
