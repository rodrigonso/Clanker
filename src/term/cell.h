#pragma once
#include "base/basics.h"
#include "base/str.h"

// A screen cell: a Unicode scalar plus a packed style. All-zero is a blank cell
// with default colours, so buffers can be cleared with memset.

enum : u8 {
  ATTR_NONE = 0,
  ATTR_BOLD = 1 << 0,
  ATTR_DIM = 1 << 1,
  ATTR_ITALIC = 1 << 2,
  ATTR_UNDERLINE = 1 << 3,
  ATTR_REVERSE = 1 << 4,
};

// 256-colour palette index, or COL_DEFAULT (0) for the terminal default. Index 0
// is reserved as "default" (so explicit palette colours use 1..255).
const u8 COL_DEFAULT = 0;

struct Style {
  u8 Fg;
  u8 Bg;
  u8 Attr;
};

inline Style StyleMake(u8 fg, u8 bg, u8 attr) { return Style{fg, bg, attr}; }
inline Style StyleDefault() { return Style{COL_DEFAULT, COL_DEFAULT, ATTR_NONE}; }
inline bool StyleEq(Style a, Style b) { return a.Fg == b.Fg && a.Bg == b.Bg && a.Attr == b.Attr; }

// Continuation marker occupying the second column of a wide (width-2) glyph.
const u32 CELL_CONT = 0xFFFFFFFFu;

struct Cell {
  u32 Ch;  // 0 = blank (rendered as space); CELL_CONT = wide-glyph tail
  Style St;
};

inline bool CellEq(Cell a, Cell b) { return a.Ch == b.Ch && StyleEq(a.St, b.St); }

// Display width of a codepoint: 0 (combining/zero-width), 2 (CJK/emoji), else 1.
u32 CharWidth(u32 cp);

// Decode one UTF-8 scalar from `s` at *i (advancing it). Returns 0 at end and
// 0xFFFD on malformed input.
u32 Utf8Decode(StrView s, u32* i);

// Total display width of a UTF-8 string (sum of CharWidth).
u32 TextWidth(StrView s);
