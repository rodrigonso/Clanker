#include "term/markdown.h"

namespace {

bool g_color = true;

Style Fg(u8 fg, u8 attr) { return Style{g_color ? fg : COL_DEFAULT, COL_DEFAULT, attr}; }
Style WithAttr(Style base, u8 attr) {
  base.Attr |= attr;
  return base;
}

// Palette choices.
Style HeadingStyle() { return Fg(75, ATTR_BOLD); }
Style CodeStyle() { return Fg(114, ATTR_NONE); }
Style LinkStyle() { return Fg(39, ATTR_UNDERLINE); }
Style BulletStyle() { return Fg(244, ATTR_NONE); }
Style QuoteStyle() { return Fg(244, ATTR_DIM); }

void PushPlain(Array<Span>* out, StrView line, u32 from, u32 to, Style base) {
  if (to > from) Array_Push(out, Span{StrView{line.Data + from, to - from}, base});
}

i32 FindChar(StrView s, u32 from, char c) {
  for (u32 i = from; i < s.Length; i++)
    if (s.Data[i] == c) return (i32)i;
  return -1;
}

i32 Find2(StrView s, u32 from, char a, char b) {
  for (u32 i = from; i + 1 < s.Length; i++)
    if (s.Data[i] == a && s.Data[i + 1] == b) return (i32)i;
  return -1;
}

u32 LeadingSpaces(StrView s) {
  u32 i = 0;
  while (i < s.Length && s.Data[i] == ' ') i++;
  return i;
}

}  // namespace

void Markdown_SetColorEnabled(bool on) { g_color = on; }

void Markdown_Inline(Arena* a, StrView line, Style base, Array<Span>* out) {
  (void)a;
  u32 i = 0;
  u32 plain = 0;
  while (i < line.Length) {
    char c = line.Data[i];

    // Bold: ** or __
    if ((c == '*' || c == '_') && i + 1 < line.Length && line.Data[i + 1] == c) {
      i32 close = Find2(line, i + 2, c, c);
      if (close > 0 && (u32)close > i + 2) {
        PushPlain(out, line, plain, i, base);
        Array_Push(out, Span{StrView{line.Data + i + 2, (u32)close - (i + 2)}, WithAttr(base, ATTR_BOLD)});
        i = (u32)close + 2;
        plain = i;
        continue;
      }
    }
    // Inline code: `code`
    if (c == '`') {
      i32 close = FindChar(line, i + 1, '`');
      if (close > 0) {
        PushPlain(out, line, plain, i, base);
        Array_Push(out, Span{StrView{line.Data + i + 1, (u32)close - (i + 1)}, CodeStyle()});
        i = (u32)close + 1;
        plain = i;
        continue;
      }
    }
    // Italic: * or _
    if (c == '*' || c == '_') {
      i32 close = FindChar(line, i + 1, c);
      if (close > 0 && (u32)close > i + 1) {
        PushPlain(out, line, plain, i, base);
        Array_Push(out, Span{StrView{line.Data + i + 1, (u32)close - (i + 1)}, WithAttr(base, ATTR_ITALIC)});
        i = (u32)close + 1;
        plain = i;
        continue;
      }
    }
    // Link: [text](url) -> render text only, styled.
    if (c == '[') {
      i32 rb = FindChar(line, i + 1, ']');
      if (rb > 0 && (u32)rb + 1 < line.Length && line.Data[rb + 1] == '(') {
        i32 rp = FindChar(line, (u32)rb + 2, ')');
        if (rp > 0) {
          PushPlain(out, line, plain, i, base);
          Array_Push(out, Span{StrView{line.Data + i + 1, (u32)rb - (i + 1)}, LinkStyle()});
          i = (u32)rp + 1;
          plain = i;
          continue;
        }
      }
    }
    i++;
  }
  PushPlain(out, line, plain, line.Length, base);
}

static void PushLine(Arena* a, Array<SpanLine>* out, Array<Span> spans) {
  (void)a;
  SpanLine sl;
  sl.Spans = spans;
  Array_Push(out, sl);
}

void Markdown_Render(Arena* a, StrView md, Array<SpanLine>* out) {
  Style def = StyleDefault();
  bool inFence = false;

  u32 i = 0;
  while (i <= md.Length) {
    // Extract one line [start, eol).
    u32 start = i;
    while (i < md.Length && md.Data[i] != '\n') i++;
    StrView ln = StrView{md.Data + start, i - start};
    bool atEnd = (i >= md.Length);
    i++;  // skip '\n'

    u32 lead = LeadingSpaces(ln);
    StrView trimmed = StrView{ln.Data + lead, ln.Length - lead};

    // Fenced code block toggling.
    if (trimmed.Length >= 3 && trimmed.Data[0] == '`' && trimmed.Data[1] == '`' &&
        trimmed.Data[2] == '`') {
      inFence = !inFence;
      if (atEnd && ln.Length == 0) break;
      continue;
    }
    if (inFence) {
      Array<Span> sp = Array_Make<Span>(a, 1);
      if (ln.Length) Array_Push(&sp, Span{ln, CodeStyle()});
      PushLine(a, out, sp);
      if (atEnd) break;
      continue;
    }

    // Heading: #..###### + space
    if (ln.Length && ln.Data[0] == '#') {
      u32 h = 0;
      while (h < ln.Length && ln.Data[h] == '#') h++;
      if (h >= 1 && h <= 6 && h < ln.Length && ln.Data[h] == ' ') {
        StrView text = StrView{ln.Data + h + 1, ln.Length - h - 1};
        Array<Span> sp = Array_Make<Span>(a, 4);
        Markdown_Inline(a, text, HeadingStyle(), &sp);
        PushLine(a, out, sp);
        if (atEnd) break;
        continue;
      }
    }

    // Unordered list: indent + (-|*|+) + space
    if (trimmed.Length >= 2 && (trimmed.Data[0] == '-' || trimmed.Data[0] == '*' || trimmed.Data[0] == '+') &&
        trimmed.Data[1] == ' ') {
      Array<Span> sp = Array_Make<Span>(a, 4);
      if (lead) Array_Push(&sp, Span{StrView{ln.Data, lead}, def});
      Array_Push(&sp, Span{SVL("\xe2\x80\xa2 "), BulletStyle()});
      Markdown_Inline(a, StrView{trimmed.Data + 2, trimmed.Length - 2}, def, &sp);
      PushLine(a, out, sp);
      if (atEnd) break;
      continue;
    }

    // Ordered list: indent + digits + (.|)) + space
    {
      u32 d = lead;
      while (d < ln.Length && ln.Data[d] >= '0' && ln.Data[d] <= '9') d++;
      if (d > lead && d + 1 < ln.Length && (ln.Data[d] == '.' || ln.Data[d] == ')') &&
          ln.Data[d + 1] == ' ') {
        Array<Span> sp = Array_Make<Span>(a, 4);
        Array_Push(&sp, Span{StrView{ln.Data, d + 2}, BulletStyle()});  // "  N. "
        Markdown_Inline(a, StrView{ln.Data + d + 2, ln.Length - d - 2}, def, &sp);
        PushLine(a, out, sp);
        if (atEnd) break;
        continue;
      }
    }

    // Blockquote: > text
    if (trimmed.Length >= 1 && trimmed.Data[0] == '>') {
      u32 off = 1;
      if (off < trimmed.Length && trimmed.Data[off] == ' ') off++;
      Array<Span> sp = Array_Make<Span>(a, 4);
      Array_Push(&sp, Span{SVL("\xe2\x94\x82 "), QuoteStyle()});
      Markdown_Inline(a, StrView{trimmed.Data + off, trimmed.Length - off}, QuoteStyle(), &sp);
      PushLine(a, out, sp);
      if (atEnd) break;
      continue;
    }

    // Paragraph / blank line.
    Array<Span> sp = Array_Make<Span>(a, 4);
    Markdown_Inline(a, ln, def, &sp);
    PushLine(a, out, sp);
    if (atEnd) break;
  }
}
