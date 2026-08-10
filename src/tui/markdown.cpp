#include "tui/markdown.h"

#include <string.h>

namespace {

// ---------------------------------------------------------------------------
// Palette and styles
// ---------------------------------------------------------------------------

// Nearest xterm-256 entries to pi's builtin dark theme. Link deliberately takes
// 110 rather than its true nearest (109) so it stays distinguishable from
// inline code, which also rounds to 109.
const MarkdownPalette kDarkPalette = {
    /*Heading*/ 222,
    /*Code*/ 109,
    /*Link*/ 110,
    /*LinkUrl*/ 241,
    /*CodeBlock*/ 143,
    /*CodeBlockBorder*/ 244,
    /*Quote*/ 245,
    /*QuoteBorder*/ 242,
    /*Hr*/ 242,
    /*ListBullet*/ 244,
    /*TableBorder*/ 244,
};

Style Tint(bool color, u8 fg, u8 attr) {
  return Style{color ? fg : COL_DEFAULT, COL_DEFAULT, attr};
}

// Recolour `base` while keeping the attributes it already carries, so a
// blockquote's italic survives an inline code span inside it.
Style Recolour(Style base, bool color, u8 fg, u8 extraAttr) {
  Style out = base;
  if (color) out.Fg = fg;
  out.Attr = (u8)(out.Attr | extraAttr);
  return out;
}

// ---------------------------------------------------------------------------
// Character classes
// ---------------------------------------------------------------------------

bool IsSpaceCh(char c) { return c == ' ' || c == '\t'; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }

bool IsPunct(char c) {
  return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
         (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

// Non-ASCII bytes are treated as word characters: they are never markdown
// syntax, and the flanking rules only need "is this a word boundary".
bool IsWordByte(char c) { return !IsSpaceCh(c) && !IsPunct(c); }

u32 LeadingSpaces(StrView s) {
  u32 i = 0;
  while (i < s.Length && s.Data[i] == ' ') i++;
  return i;
}

StrView Slice(StrView s, u32 from, u32 to) {
  if (from > s.Length) from = s.Length;
  if (to > s.Length) to = s.Length;
  if (to < from) to = from;
  return StrView{s.Data + from, to - from};
}

StrView TrimAscii(StrView s) {
  u32 a = 0;
  while (a < s.Length && IsSpaceCh(s.Data[a])) a++;
  u32 b = s.Length;
  while (b > a && IsSpaceCh(s.Data[b - 1])) b--;
  return Slice(s, a, b);
}

bool IsBlank(StrView s) {
  for (u32 i = 0; i < s.Length; i++)
    if (!IsSpaceCh(s.Data[i])) return false;
  return true;
}

StrView Repeat(Arena* a, StrView unit, u32 times) {
  if (times == 0 || unit.Length == 0) return StrView{"", 0};
  char* out = (char*)ArenaAlloc(a, (usize)unit.Length * times, 1);
  for (u32 i = 0; i < times; i++) memcpy(out + (usize)i * unit.Length, unit.Data, unit.Length);
  return StrView{out, unit.Length * times};
}

StrView SpacesOf(Arena* a, u32 count) {
  return Repeat(a, SVL(" "), count);
}

// ---------------------------------------------------------------------------
// Inline: atoms
// ---------------------------------------------------------------------------

enum : u8 { ATOM_TEXT = 0, ATOM_DELIM = 1, ATOM_STYLED = 2 };

struct MdAtom {
  StrView Text;
  Style St;       // ATOM_STYLED only
  u8 Kind;
  u8 Attr;        // emphasis attributes resolved onto this atom
  char DelimCh;
  u8 DelimLeft;   // unmatched delimiter characters still available
  bool CanOpen;
  bool CanClose;
};

void PushAtomText(Array<MdAtom>* atoms, StrView text) {
  if (text.Length == 0) return;
  MdAtom atom = {};
  atom.Kind = ATOM_TEXT;
  atom.Text = text;
  Array_Push(atoms, atom);
}

void PushAtomStyled(Array<MdAtom>* atoms, StrView text, Style st) {
  if (text.Length == 0) return;
  MdAtom atom = {};
  atom.Kind = ATOM_STYLED;
  atom.Text = text;
  atom.St = st;
  Array_Push(atoms, atom);
}

// Length of the backtick run starting at `i`.
u32 RunLength(StrView s, u32 i, char c) {
  u32 n = 0;
  while (i + n < s.Length && s.Data[i + n] == c) n++;
  return n;
}

// CommonMark code span: a run of N backticks closed by a run of exactly N.
bool ScanCodeSpan(StrView s, u32 i, u32* contentStart, u32* contentEnd, u32* next) {
  u32 open = RunLength(s, i, '`');
  u32 j = i + open;
  while (j < s.Length) {
    if (s.Data[j] != '`') {
      j++;
      continue;
    }
    u32 close = RunLength(s, j, '`');
    if (close == open) {
      u32 cs = i + open;
      u32 ce = j;
      // Strip one space from each side when both are present and the content
      // is not entirely spaces.
      if (ce > cs + 1 && s.Data[cs] == ' ' && s.Data[ce - 1] == ' ') {
        bool allSpace = true;
        for (u32 k = cs; k < ce; k++)
          if (s.Data[k] != ' ') allSpace = false;
        if (!allSpace) {
          cs++;
          ce--;
        }
      }
      *contentStart = cs;
      *contentEnd = ce;
      *next = j + close;
      return true;
    }
    j += close;
  }
  return false;
}

// Matching close bracket for the `[`/`(` at `open`, honouring nesting and
// backslash escapes. Returns the index of the closer or -1.
i32 MatchDelimiter(StrView s, u32 open, char lo, char hi) {
  u32 depth = 0;
  for (u32 i = open; i < s.Length; i++) {
    char c = s.Data[i];
    if (c == '\\') {
      i++;
      continue;
    }
    if (c == lo) depth++;
    else if (c == hi) {
      depth--;
      if (depth == 0) return (i32)i;
    }
  }
  return -1;
}

bool IsUrlStart(StrView s, u32 i, u32* schemeLen) {
  struct Prefix {
    const char* Text;
    u32 Length;
  };
  static const Prefix kPrefixes[] = {
      {"https://", 8}, {"http://", 7}, {"www.", 4}};
  for (u32 p = 0; p < sizeof(kPrefixes) / sizeof(kPrefixes[0]); p++) {
    const Prefix& pre = kPrefixes[p];
    if (i + pre.Length <= s.Length && memcmp(s.Data + i, pre.Text, pre.Length) == 0) {
      *schemeLen = pre.Length;
      return true;
    }
  }
  return false;
}

// GFM autolink extent: stop at whitespace, then trim trailing punctuation that
// is almost always sentence punctuation rather than part of the URL.
u32 BareUrlEnd(StrView s, u32 start) {
  u32 i = start;
  while (i < s.Length && !IsSpaceCh(s.Data[i]) && s.Data[i] != '<' && s.Data[i] != '>') i++;
  while (i > start) {
    char c = s.Data[i - 1];
    if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == '"' ||
        c == '\'') {
      i--;
      continue;
    }
    if (c == ')') {
      // Keep a closing paren only when it balances one inside the URL.
      u32 opens = 0, closes = 0;
      for (u32 k = start; k < i; k++) {
        if (s.Data[k] == '(') opens++;
        else if (s.Data[k] == ')') closes++;
      }
      if (closes >= opens) {
        i--;
        continue;
      }
    }
    break;
  }
  return i;
}

struct InlineCtx {
  Arena* A;
  const MarkdownPalette* Pal;
  bool Color;
  Style Base;
};

// A link destination may be wrapped in <> and may be followed by a title.
StrView ParseDestination(StrView inner) {
  StrView dest = TrimAscii(inner);
  if (dest.Length >= 2 && dest.Data[0] == '<') {
    for (u32 i = 1; i < dest.Length; i++) {
      if (dest.Data[i] == '>') return Slice(dest, 1, i);
    }
  }
  for (u32 i = 0; i < dest.Length; i++) {
    if (IsSpaceCh(dest.Data[i])) return Slice(dest, 0, i);
  }
  return dest;
}

void PushLinkAtoms(InlineCtx* ic, Array<MdAtom>* atoms, StrView text, StrView url) {
  Style linkSt = Recolour(ic->Base, ic->Color, ic->Pal->Link, ATTR_UNDERLINE);
  // Link text is emitted as one styled run rather than re-tokenised; nested
  // emphasis inside link text is rare and the extra pass is not worth it.
  PushAtomStyled(atoms, text.Length ? text : url, linkSt);
  if (url.Length == 0 || StrView_Eq(text, url)) return;
  StrView bare = url;
  if (bare.Length > 7 && memcmp(bare.Data, "mailto:", 7) == 0) bare = Slice(bare, 7, bare.Length);
  if (StrView_Eq(text, bare)) return;
  // pi's non-hyperlink fallback: " (url)". Built as one run so the line model
  // wraps it as a single token.
  StrBuf suffix = StrBuf_Make(ic->A, url.Length + 4);
  StrBuf_AppendCStr(&suffix, " (");
  StrBuf_Append(&suffix, url);
  StrBuf_AppendChar(&suffix, ')');
  PushAtomStyled(atoms, StrBuf_View(&suffix),
                 Recolour(ic->Base, ic->Color, ic->Pal->LinkUrl, ATTR_NONE));
}

void Tokenize(InlineCtx* ic, StrView s, Array<MdAtom>* atoms) {
  u32 i = 0;
  u32 plain = 0;
  while (i < s.Length) {
    char c = s.Data[i];

    // Backslash escape: the escaped punctuation becomes a literal.
    if (c == '\\' && i + 1 < s.Length && IsPunct(s.Data[i + 1])) {
      PushAtomText(atoms, Slice(s, plain, i));
      PushAtomStyled(atoms, Slice(s, i + 1, i + 2), ic->Base);
      i += 2;
      plain = i;
      continue;
    }

    // Code span: highest precedence, contents are not parsed further.
    if (c == '`') {
      u32 cs, ce, next;
      if (ScanCodeSpan(s, i, &cs, &ce, &next)) {
        PushAtomText(atoms, Slice(s, plain, i));
        PushAtomStyled(atoms, Slice(s, cs, ce),
                       Recolour(ic->Base, ic->Color, ic->Pal->Code, ATTR_NONE));
        i = next;
        plain = i;
        continue;
      }
      i += RunLength(s, i, '`');
      continue;
    }

    // Autolink: <scheme:...> or <user@host>.
    if (c == '<') {
      i32 close = -1;
      bool mail = false;
      bool scheme = false;
      for (u32 k = i + 1; k < s.Length; k++) {
        char d = s.Data[k];
        if (d == ' ' || d == '<') break;
        if (d == '@') mail = true;
        if (d == ':') scheme = true;
        if (d == '>') {
          close = (i32)k;
          break;
        }
      }
      if (close > 0 && (u32)close > i + 1 && (mail || scheme)) {
        StrView url = Slice(s, i + 1, (u32)close);
        PushAtomText(atoms, Slice(s, plain, i));
        PushAtomStyled(atoms, url, Recolour(ic->Base, ic->Color, ic->Pal->Link, ATTR_UNDERLINE));
        i = (u32)close + 1;
        plain = i;
        continue;
      }
    }

    // Image: pi renders only the alt text, unstyled.
    if (c == '!' && i + 1 < s.Length && s.Data[i + 1] == '[') {
      i32 rb = MatchDelimiter(s, i + 1, '[', ']');
      if (rb > 0 && (u32)rb + 1 < s.Length && s.Data[rb + 1] == '(') {
        i32 rp = MatchDelimiter(s, (u32)rb + 1, '(', ')');
        if (rp > 0) {
          PushAtomText(atoms, Slice(s, plain, i));
          PushAtomText(atoms, Slice(s, i + 2, (u32)rb));
          i = (u32)rp + 1;
          plain = i;
          continue;
        }
      }
    }

    // Inline link.
    if (c == '[') {
      i32 rb = MatchDelimiter(s, i, '[', ']');
      if (rb > 0 && (u32)rb + 1 < s.Length && s.Data[rb + 1] == '(') {
        i32 rp = MatchDelimiter(s, (u32)rb + 1, '(', ')');
        if (rp > 0) {
          PushAtomText(atoms, Slice(s, plain, i));
          PushLinkAtoms(ic, atoms, Slice(s, i + 1, (u32)rb),
                        ParseDestination(Slice(s, (u32)rb + 2, (u32)rp)));
          i = (u32)rp + 1;
          plain = i;
          continue;
        }
      }
    }

    // Bare URL (GFM autolink), only at a word boundary.
    {
      u32 schemeLen = 0;
      bool boundary = i == 0 || !IsWordByte(s.Data[i - 1]);
      if (boundary && IsUrlStart(s, i, &schemeLen)) {
        u32 end = BareUrlEnd(s, i);
        if (end > i + schemeLen) {
          PushAtomText(atoms, Slice(s, plain, i));
          PushAtomStyled(atoms, Slice(s, i, end),
                         Recolour(ic->Base, ic->Color, ic->Pal->Link, ATTR_UNDERLINE));
          i = end;
          plain = i;
          continue;
        }
      }
    }

    // Emphasis / strikethrough delimiter run.
    if (c == '*' || c == '_' || c == '~') {
      u32 n = RunLength(s, i, c);
      char before = i > 0 ? s.Data[i - 1] : ' ';
      char after = i + n < s.Length ? s.Data[i + n] : ' ';
      bool afterSpace = IsSpaceCh(after) || i + n >= s.Length;
      bool beforeSpace = IsSpaceCh(before) || i == 0;
      bool leftFlank = !afterSpace && (!IsPunct(after) || beforeSpace || IsPunct(before));
      bool rightFlank = !beforeSpace && (!IsPunct(before) || afterSpace || IsPunct(after));

      MdAtom atom = {};
      atom.Kind = ATOM_DELIM;
      atom.Text = Slice(s, i, i + n);
      atom.DelimCh = c;
      atom.DelimLeft = (u8)(n > 255 ? 255 : n);
      if (c == '~') {
        // pi tightens marked's rule: no whitespace or `~` adjacent to the
        // fence, and a closer may not be followed by another `~`.
        atom.CanOpen = n == 2 && !afterSpace && after != '~';
        atom.CanClose = n == 2 && !beforeSpace && before != '~' && before != '\\';
      } else if (c == '_') {
        // Intraword `_` never opens or closes, which is what keeps
        // snake_case_names from turning into emphasis.
        atom.CanOpen = leftFlank && (!rightFlank || IsPunct(before));
        atom.CanClose = rightFlank && (!leftFlank || IsPunct(after));
      } else {
        atom.CanOpen = leftFlank;
        atom.CanClose = rightFlank;
      }
      if (atom.CanOpen || atom.CanClose) {
        PushAtomText(atoms, Slice(s, plain, i));
        Array_Push(atoms, atom);
        i += n;
        plain = i;
        continue;
      }
      i += n;
      continue;
    }

    i++;
  }
  PushAtomText(atoms, Slice(s, plain, s.Length));
}

// CommonMark's "process emphasis": walk closers left to right, pair each with
// the nearest compatible opener, and stamp the resulting attribute onto every
// atom in between. The rule-of-three refinement is deliberately omitted; it
// only affects pathological runs of mixed punctuation.
void ResolveEmphasis(Array<MdAtom>* atoms) {
  for (u32 close = 0; close < atoms->Count; close++) {
    MdAtom* closer = &atoms->Data[close];
    while (closer->Kind == ATOM_DELIM && closer->CanClose && closer->DelimLeft > 0) {
      u32 openIdx = 0;
      bool found = false;
      for (u32 back = close; back > 0; back--) {
        MdAtom* candidate = &atoms->Data[back - 1];
        if (candidate->Kind != ATOM_DELIM) continue;
        if (candidate->DelimCh != closer->DelimCh) continue;
        if (!candidate->CanOpen || candidate->DelimLeft == 0) continue;
        openIdx = back - 1;
        found = true;
        break;
      }
      if (!found) break;

      MdAtom* opener = &atoms->Data[openIdx];
      u8 use;
      u8 attr;
      if (closer->DelimCh == '~') {
        use = 2;
        attr = ATTR_STRIKE;
      } else if (opener->DelimLeft >= 2 && closer->DelimLeft >= 2) {
        use = 2;
        attr = ATTR_BOLD;
      } else {
        use = 1;
        attr = ATTR_ITALIC;
      }
      if (opener->DelimLeft < use || closer->DelimLeft < use) break;

      for (u32 k = openIdx + 1; k < close; k++)
        atoms->Data[k].Attr = (u8)(atoms->Data[k].Attr | attr);
      opener->DelimLeft = (u8)(opener->DelimLeft - use);
      closer->DelimLeft = (u8)(closer->DelimLeft - use);
    }
  }
}

void EmitAtoms(const Array<MdAtom>* atoms, Style base, Array<Span>* out) {
  for (u32 i = 0; i < atoms->Count; i++) {
    const MdAtom& atom = atoms->Data[i];
    if (atom.Kind == ATOM_DELIM) {
      // Whatever was not consumed by a pairing is literal text.
      if (atom.DelimLeft == 0) continue;
      Style st = base;
      st.Attr = (u8)(st.Attr | atom.Attr);
      Array_Push(out, Span{Slice(atom.Text, 0, atom.DelimLeft), st, false});
      continue;
    }
    Style st = atom.Kind == ATOM_STYLED ? atom.St : base;
    st.Attr = (u8)(st.Attr | atom.Attr);
    Array_Push(out, Span{atom.Text, st, false});
  }
}

// ---------------------------------------------------------------------------
// Block rendering
// ---------------------------------------------------------------------------

struct MdCtx {
  Arena* A;
  Array<SpanLine>* Out;
  const MarkdownPalette* Pal;
  Style Base;
  u16 Width;
  // Extra indent for a list starting at this nesting level, so that a nested
  // list lands on pi's absolute 4-columns-per-depth grid rather than under the
  // parent's content.
  u32 ListIndent;
  bool Color;
  bool PendingBlank;
  bool EmittedAny;
};

void RenderBlocks(MdCtx* ctx, StrView md);

void PushRow(MdCtx* ctx, Array<Span> spans, Span continuation, bool hasContinuation) {
  SpanLine row = {};
  row.Spans = spans;
  if (hasContinuation) {
    row.Continuation = continuation;
    row.HasContinuation = true;
  }
  Array_Push(ctx->Out, row);
  ctx->EmittedAny = true;
}

void PushBlankRow(MdCtx* ctx) {
  SpanLine row = {};
  row.Spans = Array_Make<Span>(ctx->A, 1);
  Array_Push(ctx->Out, row);
  ctx->EmittedAny = true;
}

// One blank row is emitted between blocks, never before the first or after the
// last. Holding it pending is what keeps a streaming reply from ending on a
// blank row that appears and disappears as text arrives.
void FlushPendingBlank(MdCtx* ctx) {
  if (ctx->PendingBlank && ctx->EmittedAny) PushBlankRow(ctx);
  ctx->PendingBlank = false;
}

void PushTextRow(MdCtx* ctx, StrView text, Style style, StrView indent) {
  Array<Span> spans = Array_Make<Span>(ctx->A, 2);
  if (indent.Length) Array_Push(&spans, Span{indent, style, true});
  if (text.Length) Array_Push(&spans, Span{text, style, false});
  PushRow(ctx, spans, Span{indent, style, true}, indent.Length != 0);
}

void InlineInto(MdCtx* ctx, StrView text, Style base, Array<Span>* out) {
  InlineCtx ic;
  ic.A = ctx->A;
  ic.Pal = ctx->Pal;
  ic.Color = ctx->Color;
  ic.Base = base;

  Array<MdAtom> atoms = Array_Make<MdAtom>(ctx->A, 8);
  Tokenize(&ic, text, &atoms);
  ResolveEmphasis(&atoms);
  EmitAtoms(&atoms, base, out);
}

// ---------------------------------------------------------------------------
// Line classification
// ---------------------------------------------------------------------------

struct FenceInfo {
  u32 Indent;
  u32 Length;
  char Char;
  StrView Info;
};

bool ParseFence(StrView line, FenceInfo* out) {
  u32 indent = LeadingSpaces(line);
  if (indent > 3 || indent >= line.Length) return false;
  char c = line.Data[indent];
  if (c != '`' && c != '~') return false;
  u32 n = RunLength(line, indent, c);
  if (n < 3) return false;
  StrView info = TrimAscii(Slice(line, indent + n, line.Length));
  // An info string on a backtick fence may not itself contain a backtick.
  if (c == '`') {
    for (u32 i = 0; i < info.Length; i++)
      if (info.Data[i] == '`') return false;
  }
  out->Indent = indent;
  out->Length = n;
  out->Char = c;
  out->Info = info;
  return true;
}

bool IsFenceClose(StrView line, const FenceInfo& open) {
  u32 indent = LeadingSpaces(line);
  if (indent > 3 || indent >= line.Length) return false;
  if (line.Data[indent] != open.Char) return false;
  if (RunLength(line, indent, open.Char) < open.Length) return false;
  return IsBlank(Slice(line, indent + RunLength(line, indent, open.Char), line.Length));
}

bool ParseAtxHeading(StrView line, u32* level, StrView* text) {
  u32 indent = LeadingSpaces(line);
  if (indent > 3 || indent >= line.Length || line.Data[indent] != '#') return false;
  u32 n = RunLength(line, indent, '#');
  if (n > 6) return false;
  u32 after = indent + n;
  if (after < line.Length && !IsSpaceCh(line.Data[after])) return false;
  StrView body = TrimAscii(Slice(line, after, line.Length));
  // A closing run of `#` is decoration when preceded by a space.
  u32 end = body.Length;
  while (end > 0 && body.Data[end - 1] == '#') end--;
  if (end != body.Length && (end == 0 || IsSpaceCh(body.Data[end - 1])))
    body = TrimAscii(Slice(body, 0, end));
  *level = n;
  *text = body;
  return true;
}

bool IsHorizontalRule(StrView line) {
  u32 i = LeadingSpaces(line);
  if (i > 3 || i >= line.Length) return false;
  char c = line.Data[i];
  if (c != '-' && c != '*' && c != '_') return false;
  u32 count = 0;
  for (; i < line.Length; i++) {
    if (line.Data[i] == c) count++;
    else if (!IsSpaceCh(line.Data[i])) return false;
  }
  return count >= 3;
}

// `=` underlines a level-1 setext heading, `-` a level-2 one.
u32 SetextLevel(StrView line) {
  u32 i = LeadingSpaces(line);
  if (i > 3 || i >= line.Length) return 0;
  char c = line.Data[i];
  if (c != '=' && c != '-') return 0;
  u32 count = 0;
  for (; i < line.Length; i++) {
    if (line.Data[i] == c) count++;
    else if (!IsSpaceCh(line.Data[i])) return 0;
  }
  return count ? (c == '=' ? 1u : 2u) : 0u;
}

struct ListMarker {
  u32 Indent;       // columns before the marker
  u32 ContentCol;   // columns before the item's content
  u32 Number;       // ordered lists only
  char Delim;       // '.', ')' or the bullet character
  bool Ordered;
  bool Task;
  bool Checked;
};

bool ParseListMarker(StrView line, ListMarker* out) {
  u32 indent = LeadingSpaces(line);
  if (indent > 3 || indent >= line.Length) return false;
  if (IsHorizontalRule(line)) return false;

  u32 i = indent;
  ListMarker marker = {};
  marker.Indent = indent;
  char c = line.Data[i];
  if (c == '-' || c == '*' || c == '+') {
    marker.Ordered = false;
    marker.Delim = c;
    i++;
  } else if (IsDigit(c)) {
    u32 start = i;
    while (i < line.Length && IsDigit(line.Data[i])) i++;
    if (i - start > 9 || i >= line.Length) return false;
    if (line.Data[i] != '.' && line.Data[i] != ')') return false;
    u32 value = 0;
    for (u32 k = start; k < i; k++) value = value * 10 + (u32)(line.Data[k] - '0');
    marker.Ordered = true;
    marker.Number = value;
    marker.Delim = line.Data[i];
    i++;
  } else {
    return false;
  }

  // The marker must be followed by a space, or be the whole line.
  if (i < line.Length && !IsSpaceCh(line.Data[i])) return false;
  u32 afterMarker = i;
  while (i < line.Length && IsSpaceCh(line.Data[i])) i++;
  // A blank remainder, or more than four spaces of padding, both indent the
  // content by exactly one column past the marker.
  if (i >= line.Length || i - afterMarker > 4) i = afterMarker + 1;
  marker.ContentCol = i;

  StrView rest = Slice(line, i, line.Length);
  if (rest.Length >= 3 && rest.Data[0] == '[' && rest.Data[2] == ']' &&
      (rest.Data[1] == ' ' || rest.Data[1] == 'x' || rest.Data[1] == 'X') &&
      (rest.Length == 3 || IsSpaceCh(rest.Data[3]))) {
    marker.Task = true;
    marker.Checked = rest.Data[1] != ' ';
  }
  *out = marker;
  return true;
}

// A GFM delimiter row: cells of `-` with optional leading/trailing `:`.
bool IsTableDelimiter(StrView line, u32* columns) {
  StrView s = TrimAscii(line);
  if (s.Length == 0) return false;
  u32 i = 0;
  if (s.Data[i] == '|') i++;
  u32 count = 0;
  bool sawCell = false;
  while (i < s.Length) {
    while (i < s.Length && IsSpaceCh(s.Data[i])) i++;
    if (i < s.Length && s.Data[i] == ':') i++;
    u32 dashes = 0;
    while (i < s.Length && s.Data[i] == '-') {
      dashes++;
      i++;
    }
    if (dashes == 0) return false;
    if (i < s.Length && s.Data[i] == ':') i++;
    while (i < s.Length && IsSpaceCh(s.Data[i])) i++;
    count++;
    sawCell = true;
    if (i < s.Length) {
      if (s.Data[i] != '|') return false;
      i++;
    }
  }
  if (!sawCell) return false;
  *columns = count;
  return true;
}

u32 CountTableCells(StrView line) {
  StrView s = TrimAscii(line);
  if (s.Length == 0) return 0;
  u32 i = 0;
  if (s.Data[i] == '|') i++;
  u32 end = s.Length;
  if (end > i && s.Data[end - 1] == '|') {
    // A trailing pipe closes the last cell rather than opening a new one,
    // unless it is escaped.
    u32 backslashes = 0;
    while (end - 1 - backslashes > i && s.Data[end - 2 - backslashes] == '\\') backslashes++;
    if (backslashes % 2 == 0) end--;
  }
  u32 count = 1;
  for (u32 k = i; k < end; k++) {
    if (s.Data[k] == '\\') {
      k++;
      continue;
    }
    if (s.Data[k] == '|') count++;
  }
  return count;
}

bool IsHtmlBlockStart(StrView line) {
  u32 i = LeadingSpaces(line);
  if (i > 3 || i + 1 >= line.Length || line.Data[i] != '<') return false;
  char c = line.Data[i + 1];
  return c == '!' || c == '/' || c == '?' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// ---------------------------------------------------------------------------
// Nested rendering (blockquotes, list items, table cells)
// ---------------------------------------------------------------------------

// Renders `content` as blocks at `innerWidth`, then re-emits each produced row
// with `firstPrefix` on the first and `restPrefix` on the rest. The nested
// row arrays are abandoned in the arena, which is the arena tradeoff already
// documented in base/array.h; the waste is bounded by the block's own size.
void RenderNested(MdCtx* ctx, StrView content, Span firstPrefix, Span restPrefix,
                  StrView contPrefix, Style contStyle, u16 innerWidth, Style innerBase,
                  u32 childListIndent = 0) {
  Array<SpanLine> inner = Array_Make<SpanLine>(ctx->A, 8);
  MdCtx sub = {};
  sub.A = ctx->A;
  sub.Out = &inner;
  sub.Pal = ctx->Pal;
  sub.Base = innerBase;
  sub.Width = innerWidth;
  sub.ListIndent = childListIndent;
  sub.Color = ctx->Color;
  RenderBlocks(&sub, content);

  for (u32 i = 0; i < inner.Count; i++) {
    const SpanLine& src = inner.Data[i];
    Span prefix = i == 0 ? firstPrefix : restPrefix;
    Array<Span> spans = Array_Make<Span>(ctx->A, src.Spans.Count + 1);
    if (prefix.Text.Length) Array_Push(&spans, prefix);
    for (u32 j = 0; j < src.Spans.Count; j++) Array_Push(&spans, src.Spans.Data[j]);

    // The nested row's own continuation sits inside ours, so concatenate.
    StrView continuation = contPrefix;
    Style continuationStyle = contStyle;
    if (src.HasContinuation && src.Continuation.Text.Length) {
      StrBuf joined = StrBuf_Make(ctx->A, contPrefix.Length + src.Continuation.Text.Length + 1);
      StrBuf_Append(&joined, contPrefix);
      StrBuf_Append(&joined, src.Continuation.Text);
      continuation = StrBuf_View(&joined);
    }
    PushRow(ctx, spans, Span{continuation, continuationStyle, true}, continuation.Length != 0);
  }
}

// ---------------------------------------------------------------------------
// Span wrapping (table cells only; transcript rows are wrapped by LineModel)
// ---------------------------------------------------------------------------

void WrapSpansToWidth(Arena* a, const Array<Span>* spans, u16 width,
                      Array<Array<Span>>* rows) {
  if (width < 1) width = 1;
  Array<Span> current = Array_Make<Span>(a, 4);
  u32 col = 0;

  for (u32 s = 0; s < spans->Count; s++) {
    Span span = spans->Data[s];
    StrView text = span.Text;
    u32 i = 0;
    while (i < text.Length) {
      bool isSpace = text.Data[i] == ' ';
      u32 start = i;
      while (i < text.Length && (text.Data[i] == ' ') == isSpace) i++;
      StrView token = Slice(text, start, i);

      if (isSpace) {
        if (col == 0) continue;
        u32 room = width - col;
        u32 take = token.Length < room ? token.Length : room;
        if (take == 0) continue;
        Array_Push(&current, Span{Slice(token, 0, take), span.St, false});
        col += take;
        continue;
      }

      u32 tokenWidth = TextWidth(token);
      if (col > 0 && col + tokenWidth > width) {
        Array_Push(rows, current);
        current = Array_Make<Span>(a, 4);
        col = 0;
      }
      if (col + tokenWidth <= width) {
        Array_Push(&current, Span{token, span.St, false});
        col += tokenWidth;
        continue;
      }
      // Overlong word: break it across rows on scalar boundaries.
      u32 j = 0;
      while (j < token.Length) {
        u32 segStart = j;
        u32 segWidth = 0;
        while (j < token.Length) {
          u32 next = j;
          u32 cp = Utf8Decode(token, &next);
          u32 cw = CharWidth(cp);
          if (cw < 1) cw = 1;
          if (segWidth != 0 && col + segWidth + cw > width) break;
          segWidth += cw;
          j = next;
          if (col + segWidth >= width) break;
        }
        Array_Push(&current, Span{Slice(token, segStart, j), span.St, false});
        col += segWidth;
        if (j < token.Length) {
          Array_Push(rows, current);
          current = Array_Make<Span>(a, 4);
          col = 0;
        }
      }
    }
  }
  Array_Push(rows, current);
}

u32 SpansWidth(const Array<Span>* spans) {
  u32 total = 0;
  for (u32 i = 0; i < spans->Count; i++) total += TextWidth(spans->Data[i].Text);
  return total;
}

// ---------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------

void RenderHeading(MdCtx* ctx, u32 level, StrView text) {
  FlushPendingBlank(ctx);
  // pi strips the marker for h1/h2 and keeps a styled `### ` for h3-h6.
  u8 attr = (u8)(ATTR_BOLD | (level == 1 ? ATTR_UNDERLINE : ATTR_NONE));
  Style style = Recolour(ctx->Base, ctx->Color, ctx->Pal->Heading, attr);
  Array<Span> spans = Array_Make<Span>(ctx->A, 4);
  if (level >= 3) {
    StrBuf prefix = StrBuf_Make(ctx->A, level + 2);
    for (u32 i = 0; i < level; i++) StrBuf_AppendChar(&prefix, '#');
    StrBuf_AppendChar(&prefix, ' ');
    Array_Push(&spans, Span{StrBuf_View(&prefix), style, true});
  }
  InlineInto(ctx, text, style, &spans);
  PushRow(ctx, spans, Span{}, false);
  ctx->PendingBlank = true;
}

void RenderCodeBlock(MdCtx* ctx, StrView info, const StrView* bodyLines, u32 bodyCount,
                     u32 stripIndent) {
  FlushPendingBlank(ctx);
  Style border = Recolour(ctx->Base, ctx->Color, ctx->Pal->CodeBlockBorder, ATTR_NONE);
  Style body = Recolour(ctx->Base, ctx->Color, ctx->Pal->CodeBlock, ATTR_NONE);
  StrView indent = SVL("  ");

  // pi always prints three backticks regardless of the source fence, with the
  // info string on the opening row.
  Array<Span> open = Array_Make<Span>(ctx->A, 2);
  if (info.Length) {
    StrBuf fence = StrBuf_Make(ctx->A, info.Length + 4);
    StrBuf_AppendCStr(&fence, "```");
    StrBuf_Append(&fence, info);
    Array_Push(&open, Span{StrBuf_View(&fence), border, false});
  } else {
    Array_Push(&open, Span{SVL("```"), border, false});
  }
  PushRow(ctx, open, Span{}, false);

  for (u32 i = 0; i < bodyCount; i++) {
    StrView line = bodyLines[i];
    u32 strip = LeadingSpaces(line);
    if (strip > stripIndent) strip = stripIndent;
    PushTextRow(ctx, Slice(line, strip, line.Length), body, indent);
  }

  Array<Span> close = Array_Make<Span>(ctx->A, 1);
  Array_Push(&close, Span{SVL("```"), border, false});
  PushRow(ctx, close, Span{}, false);
}

void RenderHorizontalRule(MdCtx* ctx) {
  FlushPendingBlank(ctx);
  u32 width = ctx->Width < 80 ? ctx->Width : 80u;
  if (width < 1) width = 1;
  Array<Span> spans = Array_Make<Span>(ctx->A, 1);
  Array_Push(&spans, Span{Repeat(ctx->A, SVL("\xe2\x94\x80"), width),
                          Recolour(ctx->Base, ctx->Color, ctx->Pal->Hr, ATTR_NONE), false});
  PushRow(ctx, spans, Span{}, false);
  ctx->PendingBlank = true;
}

// Inline text whose source spanned several lines. pi keeps a soft break as a
// real break (`applyTextWithNewlines` splits on \n and rejoins on \n), so the
// inline pass runs over the whole block -- emphasis may cross a break -- and
// the resulting spans are cut at every newline.
void PushInlineRows(MdCtx* ctx, StrView text, Style base) {
  if (text.Length == 0) return;
  Array<Span> spans = Array_Make<Span>(ctx->A, 8);
  InlineInto(ctx, text, base, &spans);

  Array<Span> row = Array_Make<Span>(ctx->A, 8);
  for (u32 i = 0; i < spans.Count; i++) {
    Span sp = spans.Data[i];
    u32 start = 0;
    for (u32 j = 0; j < sp.Text.Length; j++) {
      if (sp.Text.Data[j] != '\n') continue;
      if (j > start) Array_Push(&row, Span{Slice(sp.Text, start, j), sp.St, sp.KeepLeading});
      PushRow(ctx, row, Span{}, false);
      row = Array_Make<Span>(ctx->A, 8);
      start = j + 1;
    }
    if (start < sp.Text.Length)
      Array_Push(&row, Span{Slice(sp.Text, start, sp.Text.Length), sp.St, sp.KeepLeading});
  }
  PushRow(ctx, row, Span{}, false);
}

void RenderParagraphText(MdCtx* ctx, StrView text) { PushInlineRows(ctx, text, ctx->Base); }

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

const u32 kMaxColumnWeight = 30;

void SplitTableRow(Arena* a, StrView line, u32 columns, Array<StrView>* cells) {
  StrView s = TrimAscii(line);
  u32 start = 0;
  if (s.Length && s.Data[0] == '|') start = 1;
  u32 end = s.Length;
  if (end > start && s.Data[end - 1] == '|') {
    u32 backslashes = 0;
    while (end - 1 - backslashes > start && s.Data[end - 2 - backslashes] == '\\') backslashes++;
    if (backslashes % 2 == 0) end--;
  }

  StrBuf cell = StrBuf_Make(a, 32);
  u32 emitted = 0;
  for (u32 i = start; i <= end; i++) {
    if (i == end || s.Data[i] == '|') {
      if (emitted < columns) Array_Push(cells, TrimAscii(StrBuf_View(&cell)));
      emitted++;
      cell = StrBuf_Make(a, 32);
      continue;
    }
    if (s.Data[i] == '\\' && i + 1 < end && s.Data[i + 1] == '|') {
      StrBuf_AppendChar(&cell, '|');
      i++;
      continue;
    }
    StrBuf_AppendChar(&cell, s.Data[i]);
  }
  while (cells->Count < columns) Array_Push(cells, StrView{"", 0});
}

// Longest unbroken word in a cell, which is the floor a column can shrink to.
u32 LongestWord(StrView s) {
  u32 best = 0;
  u32 i = 0;
  while (i < s.Length) {
    while (i < s.Length && IsSpaceCh(s.Data[i])) i++;
    u32 start = i;
    while (i < s.Length && !IsSpaceCh(s.Data[i])) i++;
    u32 w = TextWidth(Slice(s, start, i));
    if (w > best) best = w;
  }
  return best;
}

void RenderTable(MdCtx* ctx, const StrView* lines, u32 lineCount, u32 columns) {
  FlushPendingBlank(ctx);
  Style border = Recolour(ctx->Base, ctx->Color, ctx->Pal->TableBorder, ATTR_NONE);

  u32 rowCount = lineCount - 1;  // the delimiter row is not data
  Array<Array<StrView>> rows = Array_Make<Array<StrView>>(ctx->A, rowCount);
  for (u32 i = 0; i < lineCount; i++) {
    if (i == 1) continue;  // delimiter
    Array<StrView> cells = Array_Make<StrView>(ctx->A, columns);
    SplitTableRow(ctx->A, lines[i], columns, &cells);
    Array_Push(&rows, cells);
  }

  u32* natural = ArenaPushArray(ctx->A, u32, columns);
  u32* weight = ArenaPushArray(ctx->A, u32, columns);
  for (u32 c = 0; c < columns; c++) {
    natural[c] = 0;
    weight[c] = 1;
  }
  for (u32 r = 0; r < rows.Count; r++) {
    for (u32 c = 0; c < columns; c++) {
      StrView cell = rows.Data[r].Data[c];
      u32 w = TextWidth(cell);
      if (w > natural[c]) natural[c] = w;
      u32 word = LongestWord(cell);
      if (word > kMaxColumnWeight) word = kMaxColumnWeight;
      if (word > weight[c]) weight[c] = word;
    }
  }

  // `| ` + cell + ` ` per column, plus the closing `|`.
  u32 overhead = columns * 3 + 1;
  u32 naturalTotal = overhead;
  for (u32 c = 0; c < columns; c++) naturalTotal += natural[c];

  u32* width = ArenaPushArray(ctx->A, u32, columns);
  if (naturalTotal <= ctx->Width) {
    for (u32 c = 0; c < columns; c++) width[c] = natural[c] ? natural[c] : 1;
  } else if (ctx->Width <= overhead || ctx->Width - overhead < columns) {
    // Too narrow for even one column each: fall back to the raw source, which
    // is what pi does.
    for (u32 i = 0; i < lineCount; i++) RenderParagraphText(ctx, TrimAscii(lines[i]));
    ctx->PendingBlank = true;
    return;
  } else {
    u32 available = ctx->Width - overhead;
    u32 weightTotal = 0;
    for (u32 c = 0; c < columns; c++) weightTotal += weight[c];
    u32 assigned = 0;
    for (u32 c = 0; c < columns; c++) {
      u32 share = weightTotal ? (available * weight[c]) / weightTotal : available / columns;
      if (share < 1) share = 1;
      if (share > natural[c]) share = natural[c] ? natural[c] : 1;
      width[c] = share;
      assigned += share;
    }
    // Hand the rounding remainder to columns that are still clipped.
    for (u32 c = 0; c < columns && assigned < available; c++) {
      while (assigned < available && width[c] < natural[c]) {
        width[c]++;
        assigned++;
      }
    }
  }

  StrView dash = SVL("\xe2\x94\x80");
  struct Rule {
    const char* Left;
    const char* Mid;
    const char* Right;
  };
  const Rule kTop = {"\xe2\x94\x8c", "\xe2\x94\xac", "\xe2\x94\x90"};
  const Rule kSep = {"\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4"};
  const Rule kBottom = {"\xe2\x94\x94", "\xe2\x94\xb4", "\xe2\x94\x98"};

  auto pushRule = [&](const Rule& rule) {
    StrBuf line = StrBuf_Make(ctx->A, ctx->Width + 16);
    StrBuf_AppendCStr(&line, rule.Left);
    for (u32 c = 0; c < columns; c++) {
      if (c) StrBuf_AppendCStr(&line, rule.Mid);
      StrBuf_Append(&line, Repeat(ctx->A, dash, width[c] + 2));
    }
    StrBuf_AppendCStr(&line, rule.Right);
    Array<Span> spans = Array_Make<Span>(ctx->A, 1);
    Array_Push(&spans, Span{StrBuf_View(&line), border, true});
    PushRow(ctx, spans, Span{}, false);
  };

  pushRule(kTop);
  for (u32 r = 0; r < rows.Count; r++) {
    bool header = r == 0;
    Style cellBase = ctx->Base;
    if (header) cellBase.Attr = (u8)(cellBase.Attr | ATTR_BOLD);

    // Render every cell, wrap it to its column, and pad all cells in the row
    // out to the tallest one.
    Array<Array<Array<Span>>> wrapped = Array_Make<Array<Array<Span>>>(ctx->A, columns);
    u32 height = 1;
    for (u32 c = 0; c < columns; c++) {
      Array<Span> spans = Array_Make<Span>(ctx->A, 4);
      InlineInto(ctx, rows.Data[r].Data[c], cellBase, &spans);
      Array<Array<Span>> cellRows = Array_Make<Array<Span>>(ctx->A, 1);
      WrapSpansToWidth(ctx->A, &spans, (u16)width[c], &cellRows);
      Array_Push(&wrapped, cellRows);
      if (cellRows.Count > height) height = cellRows.Count;
    }

    for (u32 line = 0; line < height; line++) {
      Array<Span> spans = Array_Make<Span>(ctx->A, columns * 3 + 2);
      Array_Push(&spans, Span{SVL("\xe2\x94\x82 "), border, true});
      for (u32 c = 0; c < columns; c++) {
        if (c) Array_Push(&spans, Span{SVL(" \xe2\x94\x82 "), border, true});
        u32 used = 0;
        if (line < wrapped.Data[c].Count) {
          Array<Span>* cellRow = &wrapped.Data[c].Data[line];
          for (u32 k = 0; k < cellRow->Count; k++) {
            Span sp = cellRow->Data[k];
            sp.KeepLeading = true;
            Array_Push(&spans, sp);
          }
          used = SpansWidth(cellRow);
        }
        if (used < width[c])
          Array_Push(&spans, Span{SpacesOf(ctx->A, width[c] - used), cellBase, true});
      }
      Array_Push(&spans, Span{SVL(" \xe2\x94\x82"), border, true});
      PushRow(ctx, spans, Span{}, false);
    }
    if (header) pushRule(kSep);
  }
  pushRule(kBottom);
  ctx->PendingBlank = true;
}

// ---------------------------------------------------------------------------
// Lists
// ---------------------------------------------------------------------------

const u32 kListIndent = 4;

// A line belongs to the current item when it is indented to the item's content
// column, or when it is a lazy paragraph continuation.
bool IsItemContinuation(StrView line, u32 contentCol) {
  if (IsBlank(line)) return true;
  return LeadingSpaces(line) >= contentCol;
}

void RenderList(MdCtx* ctx, const Array<StrView>* lines, u32* index);

// Collects one item's raw content (marker stripped, continuation lines
// dedented) and renders it through RenderNested so nesting, code blocks and
// tables inside list items all work.
void RenderListItem(MdCtx* ctx, const Array<StrView>* lines, u32* index, const ListMarker& marker,
                    u32 displayNumber, bool ordered) {
  StrBuf content = StrBuf_Make(ctx->A, 128);
  StrView first = lines->Data[*index];
  u32 contentStart = marker.ContentCol;
  if (marker.Task) {
    // The checkbox becomes part of the marker, so it is not item content.
    StrView rest = Slice(first, contentStart, first.Length);
    u32 skip = 3;
    while (skip < rest.Length && IsSpaceCh(rest.Data[skip])) skip++;
    contentStart += skip;
  }
  StrBuf_Append(&content, Slice(first, contentStart, first.Length));
  (*index)++;

  u32 pendingBlanks = 0;
  while (*index < lines->Count) {
    StrView line = lines->Data[*index];
    if (IsBlank(line)) {
      // A blank only stays in the item if more item content follows.
      u32 look = *index;
      while (look < lines->Count && IsBlank(lines->Data[look])) look++;
      if (look >= lines->Count || LeadingSpaces(lines->Data[look]) < marker.ContentCol) break;
      pendingBlanks++;
      (*index)++;
      continue;
    }
    ListMarker next;
    bool startsItem = ParseListMarker(line, &next);
    if (startsItem && next.Indent < marker.ContentCol) break;
    if (!IsItemContinuation(line, marker.ContentCol) && (startsItem || IsHorizontalRule(line)))
      break;
    if (LeadingSpaces(line) < marker.ContentCol && pendingBlanks) break;

    while (pendingBlanks) {
      StrBuf_AppendChar(&content, '\n');
      pendingBlanks--;
    }
    StrBuf_AppendChar(&content, '\n');
    u32 strip = LeadingSpaces(line);
    if (strip > marker.ContentCol) strip = marker.ContentCol;
    StrBuf_Append(&content, Slice(line, strip, line.Length));
    (*index)++;
  }

  StrBuf label = StrBuf_Make(ctx->A, 16);
  StrBuf_Append(&label, SpacesOf(ctx->A, ctx->ListIndent));
  if (ordered) {
    StrBuf_AppendU64(&label, displayNumber);
    StrBuf_AppendChar(&label, marker.Delim == ')' ? ')' : '.');
    StrBuf_AppendChar(&label, ' ');
  } else {
    // pi normalises every bullet character to "- ".
    StrBuf_AppendCStr(&label, "- ");
  }
  if (marker.Task) StrBuf_AppendCStr(&label, marker.Checked ? "[x] " : "[ ] ");

  StrView markerText = StrBuf_View(&label);
  u32 markerWidth = TextWidth(markerText);
  // pi puts a nested list on an absolute 4-column grid, so a child list adds
  // whatever this marker did not. A marker wider than the grid step (`- [x] `,
  // or `100. `) would need a negative adjustment; there the child stays under
  // the parent's content rather than backing out to the left of it.
  u32 ownWidth = markerWidth - ctx->ListIndent;
  u32 childIndent = ownWidth < 4 ? 4 - ownWidth : 0;
  Style bullet = Recolour(ctx->Base, ctx->Color, ctx->Pal->ListBullet, ATTR_NONE);
  StrView pad = SpacesOf(ctx->A, markerWidth);
  u16 innerWidth = ctx->Width > markerWidth ? (u16)(ctx->Width - markerWidth) : (u16)1;

  RenderNested(ctx, StrBuf_View(&content), Span{markerText, bullet, true},
               Span{pad, ctx->Base, true}, pad, ctx->Base, innerWidth, ctx->Base, childIndent);
}

void RenderList(MdCtx* ctx, const Array<StrView>* lines, u32* index) {
  FlushPendingBlank(ctx);
  ListMarker first;
  if (!ParseListMarker(lines->Data[*index], &first)) return;
  bool ordered = first.Ordered;
  u32 number = ordered ? first.Number : 0;
  u32 baseIndent = first.Indent;

  bool loose = false;
  bool firstItem = true;
  while (*index < lines->Count) {
    StrView line = lines->Data[*index];
    if (IsBlank(line)) {
      u32 look = *index;
      while (look < lines->Count && IsBlank(lines->Data[look])) look++;
      ListMarker peek;
      if (look >= lines->Count || !ParseListMarker(lines->Data[look], &peek) ||
          peek.Indent != baseIndent || peek.Ordered != ordered) {
        break;
      }
      loose = true;
      *index = look;
      continue;
    }
    ListMarker marker;
    if (!ParseListMarker(line, &marker) || marker.Indent != baseIndent ||
        marker.Ordered != ordered) {
      break;
    }
    if (!firstItem && loose) PushBlankRow(ctx);
    RenderListItem(ctx, lines, index, marker, number, ordered);
    if (ordered) number++;
    firstItem = false;
  }
  ctx->PendingBlank = true;
}

// ---------------------------------------------------------------------------
// Blockquotes
// ---------------------------------------------------------------------------

bool IsQuoteLine(StrView line) {
  u32 i = LeadingSpaces(line);
  return i <= 3 && i < line.Length && line.Data[i] == '>';
}

void RenderQuote(MdCtx* ctx, const Array<StrView>* lines, u32* index) {
  FlushPendingBlank(ctx);
  StrBuf content = StrBuf_Make(ctx->A, 128);
  bool first = true;
  while (*index < lines->Count) {
    StrView line = lines->Data[*index];
    if (IsQuoteLine(line)) {
      u32 i = LeadingSpaces(line) + 1;
      if (i < line.Length && line.Data[i] == ' ') i++;
      if (!first) StrBuf_AppendChar(&content, '\n');
      StrBuf_Append(&content, Slice(line, i, line.Length));
      first = false;
      (*index)++;
      continue;
    }
    // Lazy continuation: a plain paragraph line directly under a quoted one.
    if (!first && !IsBlank(line) && !IsHorizontalRule(line)) {
      ListMarker ignored;
      FenceInfo fence;
      u32 level;
      StrView text;
      if (!ParseListMarker(line, &ignored) && !ParseFence(line, &fence) &&
          !ParseAtxHeading(line, &level, &text)) {
        StrBuf_AppendChar(&content, '\n');
        StrBuf_Append(&content, line);
        (*index)++;
        continue;
      }
    }
    break;
  }

  Style borderStyle = Recolour(ctx->Base, ctx->Color, ctx->Pal->QuoteBorder, ATTR_NONE);
  Style quoteBase = Recolour(ctx->Base, ctx->Color, ctx->Pal->Quote, ATTR_ITALIC);
  StrView bar = SVL("\xe2\x94\x82 ");
  u16 innerWidth = ctx->Width > 2 ? (u16)(ctx->Width - 2) : (u16)1;
  Span barSpan = Span{bar, borderStyle, true};
  RenderNested(ctx, StrBuf_View(&content), barSpan, barSpan, bar, borderStyle, innerWidth,
               quoteBase);
  ctx->PendingBlank = true;
}

// ---------------------------------------------------------------------------
// Block dispatch
// ---------------------------------------------------------------------------

bool IsTableStart(const Array<StrView>* lines, u32 index, u32* columns) {
  if (index + 1 >= lines->Count) return false;
  StrView header = lines->Data[index];
  if (LeadingSpaces(header) > 3) return false;
  u32 headerCells = CountTableCells(header);
  if (headerCells < 2) return false;
  bool hasPipe = false;
  for (u32 i = 0; i < header.Length; i++)
    if (header.Data[i] == '|') hasPipe = true;
  if (!hasPipe) return false;
  u32 delimCells = 0;
  if (!IsTableDelimiter(lines->Data[index + 1], &delimCells)) return false;
  if (delimCells != headerCells) return false;
  *columns = headerCells;
  return true;
}

// Does a new block start here? Used to terminate paragraph accumulation.
bool StartsNewBlock(const Array<StrView>* lines, u32 index) {
  StrView line = lines->Data[index];
  FenceInfo fence;
  ListMarker marker;
  u32 level;
  StrView text;
  u32 columns;
  if (ParseFence(line, &fence)) return true;
  if (ParseAtxHeading(line, &level, &text)) return true;
  if (IsHorizontalRule(line)) return true;
  if (IsQuoteLine(line)) return true;
  if (ParseListMarker(line, &marker)) return true;
  if (IsHtmlBlockStart(line)) return true;
  if (IsTableStart(lines, index, &columns)) return true;
  return false;
}

// A trailing run of 2+ spaces, or a trailing backslash, is a hard break.
bool HasHardBreak(StrView line, StrView* trimmed) {
  if (line.Length >= 1 && line.Data[line.Length - 1] == '\\') {
    *trimmed = Slice(line, 0, line.Length - 1);
    return true;
  }
  u32 spaces = 0;
  while (spaces < line.Length && line.Data[line.Length - 1 - spaces] == ' ') spaces++;
  if (spaces >= 2) {
    *trimmed = Slice(line, 0, line.Length - spaces);
    return true;
  }
  return false;
}

void RenderParagraph(MdCtx* ctx, const Array<StrView>* lines, u32* index) {
  Array<StrView> collected = Array_Make<StrView>(ctx->A, 4);
  u32 setext = 0;

  while (*index < lines->Count) {
    StrView line = lines->Data[*index];
    if (IsBlank(line)) break;
    if (collected.Count) {
      u32 level = SetextLevel(line);
      // `---` under a paragraph is a setext underline, not a rule.
      if (level) {
        setext = level;
        (*index)++;
        break;
      }
      if (StartsNewBlock(lines, *index)) break;
    }
    Array_Push(&collected, line);
    (*index)++;
  }
  if (collected.Count == 0) return;

  if (setext) {
    StrBuf joined = StrBuf_Make(ctx->A, 128);
    for (u32 i = 0; i < collected.Count; i++) {
      if (i) StrBuf_AppendChar(&joined, ' ');
      StrBuf_Append(&joined, TrimAscii(collected.Data[i]));
    }
    RenderHeading(ctx, setext, StrBuf_View(&joined));
    return;
  }

  FlushPendingBlank(ctx);
  // Each source line stays its own row, matching pi: only the hard-break
  // markers themselves are stripped.
  StrBuf joined = StrBuf_Make(ctx->A, 128);
  for (u32 i = 0; i < collected.Count; i++) {
    StrView line = collected.Data[i];
    StrView beforeBreak;
    if (HasHardBreak(line, &beforeBreak)) line = beforeBreak;
    if (i) StrBuf_AppendChar(&joined, '\n');
    StrBuf_Append(&joined, TrimAscii(line));
  }
  RenderParagraphText(ctx, StrBuf_View(&joined));
  // pi omits the trailing blank when a list follows.
  ctx->PendingBlank = !(*index < lines->Count && StartsNewBlock(lines, *index) &&
                        [&] {
                          ListMarker marker;
                          return ParseListMarker(lines->Data[*index], &marker);
                        }());
}

void RenderFencedCode(MdCtx* ctx, const Array<StrView>* lines, u32* index) {
  FenceInfo fence;
  ParseFence(lines->Data[*index], &fence);
  (*index)++;
  u32 start = *index;
  while (*index < lines->Count && !IsFenceClose(lines->Data[*index], fence)) (*index)++;
  u32 count = *index - start;
  if (*index < lines->Count) (*index)++;  // closing fence
  RenderCodeBlock(ctx, fence.Info, lines->Data + start, count, fence.Indent);
}

void RenderIndentedCode(MdCtx* ctx, const Array<StrView>* lines, u32* index) {
  u32 start = *index;
  u32 end = *index;
  while (*index < lines->Count) {
    StrView line = lines->Data[*index];
    if (IsBlank(line)) {
      (*index)++;
      continue;
    }
    if (LeadingSpaces(line) < 4) break;
    (*index)++;
    end = *index;
  }
  *index = end;
  RenderCodeBlock(ctx, StrView{"", 0}, lines->Data + start, end - start, 4);
}

void RenderHtmlBlock(MdCtx* ctx, const Array<StrView>* lines, u32* index) {
  FlushPendingBlank(ctx);
  while (*index < lines->Count && !IsBlank(lines->Data[*index])) {
    PushTextRow(ctx, lines->Data[*index], ctx->Base, StrView{"", 0});
    (*index)++;
  }
  ctx->PendingBlank = true;
}

void RenderTableBlock(MdCtx* ctx, const Array<StrView>* lines, u32* index, u32 columns) {
  u32 start = *index;
  *index += 2;  // header + delimiter
  while (*index < lines->Count) {
    StrView line = lines->Data[*index];
    if (IsBlank(line)) break;
    bool hasPipe = false;
    for (u32 i = 0; i < line.Length; i++)
      if (line.Data[i] == '|') hasPipe = true;
    if (!hasPipe) break;
    (*index)++;
  }
  RenderTable(ctx, lines->Data + start, *index - start, columns);
}

void RenderBlocks(MdCtx* ctx, StrView md) {
  Array<StrView> lines = Array_Make<StrView>(ctx->A, 16);
  u32 i = 0;
  while (i <= md.Length) {
    u32 start = i;
    while (i < md.Length && md.Data[i] != '\n') i++;
    u32 end = i;
    if (end > start && md.Data[end - 1] == '\r') end--;
    Array_Push(&lines, Slice(md, start, end));
    if (i >= md.Length) break;
    i++;
  }
  // A trailing newline yields a final empty line that is not a real blank.
  if (lines.Count && md.Length && md.Data[md.Length - 1] == '\n' &&
      lines.Data[lines.Count - 1].Length == 0) {
    lines.Count--;
  }

  u32 index = 0;
  while (index < lines.Count) {
    StrView line = lines.Data[index];
    if (IsBlank(line)) {
      ctx->PendingBlank = true;
      index++;
      continue;
    }

    FenceInfo fence;
    if (ParseFence(line, &fence)) {
      RenderFencedCode(ctx, &lines, &index);
      continue;
    }
    u32 level;
    StrView text;
    if (ParseAtxHeading(line, &level, &text)) {
      RenderHeading(ctx, level, text);
      index++;
      continue;
    }
    if (IsHorizontalRule(line)) {
      RenderHorizontalRule(ctx);
      index++;
      continue;
    }
    if (IsQuoteLine(line)) {
      RenderQuote(ctx, &lines, &index);
      continue;
    }
    ListMarker marker;
    if (ParseListMarker(line, &marker)) {
      RenderList(ctx, &lines, &index);
      continue;
    }
    u32 columns;
    if (IsTableStart(&lines, index, &columns)) {
      RenderTableBlock(ctx, &lines, &index, columns);
      continue;
    }
    if (LeadingSpaces(line) >= 4) {
      RenderIndentedCode(ctx, &lines, &index);
      continue;
    }
    if (IsHtmlBlockStart(line)) {
      RenderHtmlBlock(ctx, &lines, &index);
      continue;
    }
    RenderParagraph(ctx, &lines, &index);
  }
}

// ---------------------------------------------------------------------------
// Input normalisation
// ---------------------------------------------------------------------------

// pi's trimPartialClosingFences: while a fence is open and the model has begun
// typing its closing run, drop the partial run so the block does not visibly
// shrink and regrow as the backticks arrive one at a time.
StrView TrimPartialClosingFence(StrView md) {
  // Find the last unclosed fence by replaying opens and closes.
  bool open = false;
  char fenceChar = '`';
  u32 fenceLen = 0;
  u32 lastLineStart = 0;
  u32 i = 0;
  while (i <= md.Length) {
    u32 start = i;
    while (i < md.Length && md.Data[i] != '\n') i++;
    u32 end = i;
    if (end > start && md.Data[end - 1] == '\r') end--;
    StrView line = Slice(md, start, end);
    lastLineStart = start;

    if (!open) {
      FenceInfo info;
      if (ParseFence(line, &info)) {
        open = true;
        fenceChar = info.Char;
        fenceLen = info.Length;
      }
    } else {
      FenceInfo probe;
      probe.Char = fenceChar;
      probe.Length = fenceLen;
      probe.Indent = 0;
      probe.Info = StrView{"", 0};
      if (IsFenceClose(line, probe)) open = false;
    }
    if (i >= md.Length) break;
    i++;
  }
  if (!open || fenceLen == 0) return md;

  StrView last = Slice(md, lastLineStart, md.Length);
  if (last.Length == 0 || last.Length >= fenceLen) return md;
  for (u32 k = 0; k < last.Length; k++)
    if (last.Data[k] != fenceChar) return md;
  // Drop the partial run and the newline that precedes it.
  u32 cut = lastLineStart;
  if (cut > 0 && md.Data[cut - 1] == '\n') cut--;
  return Slice(md, 0, cut);
}

// pi normalises tabs to three spaces before tokenizing.
StrView NormalizeTabs(Arena* a, StrView md) {
  bool hasTab = false;
  for (u32 i = 0; i < md.Length; i++)
    if (md.Data[i] == '\t') hasTab = true;
  if (!hasTab) return md;
  StrBuf out = StrBuf_Make(a, md.Length + 32);
  for (u32 i = 0; i < md.Length; i++) {
    if (md.Data[i] == '\t') StrBuf_AppendCStr(&out, "   ");
    else StrBuf_AppendChar(&out, md.Data[i]);
  }
  return StrBuf_View(&out);
}

}  // namespace

const MarkdownPalette& Markdown_DefaultPalette() { return kDarkPalette; }

void Markdown_Inline(Arena* a, StrView line, Style base, Array<Span>* out, bool color) {
  InlineCtx ic;
  ic.A = a;
  ic.Pal = &kDarkPalette;
  ic.Color = color;
  ic.Base = base;

  Array<MdAtom> atoms = Array_Make<MdAtom>(a, 8);
  Tokenize(&ic, line, &atoms);
  ResolveEmphasis(&atoms);
  EmitAtoms(&atoms, base, out);
}

void Markdown_Render(Arena* a, StrView md, Array<SpanLine>* out, MarkdownOpts opts) {
  MdCtx ctx = {};
  ctx.A = a;
  ctx.Out = out;
  ctx.Pal = &kDarkPalette;
  ctx.Base = StyleDefault();
  ctx.Width = opts.Width ? opts.Width : (u16)80;
  ctx.Color = opts.Color;
  RenderBlocks(&ctx, NormalizeTabs(a, TrimPartialClosingFence(md)));
}
