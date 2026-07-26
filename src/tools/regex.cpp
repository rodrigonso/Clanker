#include "tools/regex.h"

#include <string.h>

static const u32 REGEX_INF = 0xffffffffu;

enum RegexNodeKind {
  RegexNode_Empty,
  RegexNode_Literal,
  RegexNode_Any,
  RegexNode_Class,
  RegexNode_Start,
  RegexNode_End,
  RegexNode_Seq,
  RegexNode_Alt,
  RegexNode_Repeat,
};

enum RegexClassAtomKind {
  RegexClassAtom_Range,
  RegexClassAtom_Digit,
  RegexClassAtom_NotDigit,
  RegexClassAtom_Word,
  RegexClassAtom_NotWord,
  RegexClassAtom_Space,
  RegexClassAtom_NotSpace,
};

struct RegexClassAtom {
  u32 Kind;
  u8 Lo;
  u8 Hi;
};

struct RegexClass {
  bool Negate;
  u32 Count;
  RegexClassAtom* Atoms;
};

struct RegexNode {
  u32 Kind;
  union {
    u8 Literal;
    RegexClass* Class;
    struct {
      RegexNode** Items;
      u32 Count;
    } Seq;
    struct {
      RegexNode* Left;
      RegexNode* Right;
    } Alt;
    struct {
      RegexNode* Child;
      u32 Min;
      u32 Max;
      bool Lazy;
    } Repeat;
  };
};

struct Regex {
  RegexNode* Root;
  bool CaseInsensitive;
  u32 PatternLength;
};

struct RegexParser {
  Arena* Arena;
  StrView Pattern;
  u32 Pos;
  StrBuf* Err;
  bool Failed;
};

struct RegexContext;

enum RegexContKind {
  RegexCont_Success,
  RegexCont_Seq,
  RegexCont_Repeat,
};

struct RegexCont {
  u32 Kind;
  const RegexCont* Parent;
  const RegexNode* SeqNode;
  u32 Index;
  const RegexNode* RepeatNode;
  u32 Count;
  u32 PrevPos;
};

struct RegexContext {
  const Regex* Re;
  StrView Text;
  u32 Steps;
  u32 StepLimit;
  u32 Depth;
  u32 DepthLimit;
};

static bool Regex_IsDigit(u8 c) { return c >= '0' && c <= '9'; }
static bool Regex_IsAlpha(u8 c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static bool Regex_IsWord(u8 c) { return Regex_IsAlpha(c) || Regex_IsDigit(c) || c == '_'; }
static bool Regex_IsSpace(u8 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

static u8 Regex_Fold(u8 c) {
  if (c >= 'A' && c <= 'Z') return (u8)(c + ('a' - 'A'));
  return c;
}

static void Regex_SetErr(RegexParser* p, const char* msg) {
  if (!p->Failed && p->Err) StrBuf_AppendCStr(p->Err, msg);
  p->Failed = true;
}

static RegexNode* Regex_NewNode(RegexParser* p, u32 kind) {
  RegexNode* n = ArenaPushStruct(p->Arena, RegexNode);
  memset(n, 0, sizeof(*n));
  n->Kind = kind;
  return n;
}

static RegexNode* Regex_NewEmpty(RegexParser* p) { return Regex_NewNode(p, RegexNode_Empty); }

static RegexNode* Regex_NewLiteral(RegexParser* p, u8 c) {
  RegexNode* n = Regex_NewNode(p, RegexNode_Literal);
  n->Literal = c;
  return n;
}

static RegexNode* Regex_NewClassNode(RegexParser* p, RegexClass* c) {
  RegexNode* n = Regex_NewNode(p, RegexNode_Class);
  n->Class = c;
  return n;
}

static bool Regex_AtEnd(RegexParser* p) { return p->Pos >= p->Pattern.Length; }
static u8 Regex_Peek(RegexParser* p) { return Regex_AtEnd(p) ? 0 : (u8)p->Pattern.Data[p->Pos]; }

static u8 Regex_Get(RegexParser* p) {
  if (Regex_AtEnd(p)) return 0;
  return (u8)p->Pattern.Data[p->Pos++];
}

static RegexClass* Regex_NewClass(RegexParser* p, u32 maxAtoms) {
  RegexClass* c = ArenaPushStruct(p->Arena, RegexClass);
  c->Negate = false;
  c->Count = 0;
  c->Atoms = ArenaPushArray(p->Arena, RegexClassAtom, maxAtoms ? maxAtoms : 1);
  return c;
}

static void Regex_ClassAdd(RegexClass* c, u32 kind, u8 lo, u8 hi) {
  c->Atoms[c->Count].Kind = kind;
  c->Atoms[c->Count].Lo = lo;
  c->Atoms[c->Count].Hi = hi;
  c->Count++;
}

static RegexClass* Regex_ClassForKind(RegexParser* p, u32 kind) {
  RegexClass* c = Regex_NewClass(p, 1);
  Regex_ClassAdd(c, kind, 0, 0);
  return c;
}

static bool Regex_ParseEscape(RegexParser* p, RegexNode** outNode, u8* outLiteral) {
  if (Regex_AtEnd(p)) {
    Regex_SetErr(p, "trailing escape");
    return false;
  }

  u8 c = Regex_Get(p);
  *outNode = nullptr;
  *outLiteral = c;
  switch (c) {
    case 'd': *outNode = Regex_NewClassNode(p, Regex_ClassForKind(p, RegexClassAtom_Digit)); return true;
    case 'D': *outNode = Regex_NewClassNode(p, Regex_ClassForKind(p, RegexClassAtom_NotDigit)); return true;
    case 'w': *outNode = Regex_NewClassNode(p, Regex_ClassForKind(p, RegexClassAtom_Word)); return true;
    case 'W': *outNode = Regex_NewClassNode(p, Regex_ClassForKind(p, RegexClassAtom_NotWord)); return true;
    case 's': *outNode = Regex_NewClassNode(p, Regex_ClassForKind(p, RegexClassAtom_Space)); return true;
    case 'S': *outNode = Regex_NewClassNode(p, Regex_ClassForKind(p, RegexClassAtom_NotSpace)); return true;
    case 'n': *outLiteral = '\n'; return true;
    case 't': *outLiteral = '\t'; return true;
    case 'r': *outLiteral = '\r'; return true;
    default: return true;
  }
}

struct RegexClassItem {
  bool IsLiteral;
  u8 Literal;
  u32 Kind;
};

static bool Regex_ParseClassItem(RegexParser* p, RegexClassItem* out) {
  if (Regex_AtEnd(p)) {
    Regex_SetErr(p, "bad character class");
    return false;
  }

  u8 c = Regex_Get(p);
  if (c == '\\') {
    if (Regex_AtEnd(p)) {
      Regex_SetErr(p, "bad character class");
      return false;
    }
    c = Regex_Get(p);
    out->IsLiteral = false;
    out->Kind = RegexClassAtom_Range;
    out->Literal = c;
    switch (c) {
      case 'd': out->Kind = RegexClassAtom_Digit; return true;
      case 'D': out->Kind = RegexClassAtom_NotDigit; return true;
      case 'w': out->Kind = RegexClassAtom_Word; return true;
      case 'W': out->Kind = RegexClassAtom_NotWord; return true;
      case 's': out->Kind = RegexClassAtom_Space; return true;
      case 'S': out->Kind = RegexClassAtom_NotSpace; return true;
      case 'n': out->IsLiteral = true; out->Literal = '\n'; return true;
      case 't': out->IsLiteral = true; out->Literal = '\t'; return true;
      case 'r': out->IsLiteral = true; out->Literal = '\r'; return true;
      default: out->IsLiteral = true; out->Literal = c; return true;
    }
  }

  out->IsLiteral = true;
  out->Literal = c;
  out->Kind = RegexClassAtom_Range;
  return true;
}

static RegexNode* Regex_ParseExpression(RegexParser* p);

static RegexNode* Regex_ParseClass(RegexParser* p) {
  Regex_Get(p);  // [
  RegexClass* c = Regex_NewClass(p, p->Pattern.Length + 1);
  if (Regex_Peek(p) == '^') {
    c->Negate = true;
    p->Pos++;
  }

  bool any = false;
  while (!Regex_AtEnd(p)) {
    if (Regex_Peek(p) == ']' && any) {
      p->Pos++;
      return Regex_NewClassNode(p, c);
    }

    RegexClassItem first;
    if (!Regex_ParseClassItem(p, &first)) return nullptr;
    any = true;

    if (first.IsLiteral && Regex_Peek(p) == '-' && p->Pos + 1 < p->Pattern.Length && p->Pattern.Data[p->Pos + 1] != ']') {
      p->Pos++;
      RegexClassItem second;
      if (!Regex_ParseClassItem(p, &second)) return nullptr;
      if (!second.IsLiteral) {
        Regex_SetErr(p, "bad character class range");
        return nullptr;
      }
      if (first.Literal > second.Literal) {
        Regex_SetErr(p, "bad character class range");
        return nullptr;
      }
      Regex_ClassAdd(c, RegexClassAtom_Range, first.Literal, second.Literal);
    } else if (first.IsLiteral) {
      Regex_ClassAdd(c, RegexClassAtom_Range, first.Literal, first.Literal);
    } else {
      Regex_ClassAdd(c, first.Kind, 0, 0);
    }
  }

  Regex_SetErr(p, "unclosed character class");
  return nullptr;
}

static bool Regex_ParseNumber(RegexParser* p, u32* out) {
  if (!Regex_IsDigit(Regex_Peek(p))) return false;
  u32 v = 0;
  while (Regex_IsDigit(Regex_Peek(p))) {
    u32 d = (u32)(Regex_Get(p) - '0');
    if (v > (REGEX_INF - d) / 10) v = REGEX_INF;
    else v = v * 10 + d;
  }
  *out = v;
  return true;
}

static RegexNode* Regex_ParseAtom(RegexParser* p) {
  if (Regex_AtEnd(p)) return Regex_NewEmpty(p);

  u8 c = Regex_Peek(p);
  if (c == '|' || c == ')') return Regex_NewEmpty(p);
  if (c == '*' || c == '+' || c == '?' || c == '{') {
    Regex_SetErr(p, "nothing to repeat");
    return nullptr;
  }

  if (c == '.') {
    p->Pos++;
    return Regex_NewNode(p, RegexNode_Any);
  }
  if (c == '^') {
    p->Pos++;
    return Regex_NewNode(p, RegexNode_Start);
  }
  if (c == '$') {
    p->Pos++;
    return Regex_NewNode(p, RegexNode_End);
  }
  if (c == '[') return Regex_ParseClass(p);
  if (c == '(') {
    p->Pos++;
    RegexNode* inside = Regex_ParseExpression(p);
    if (p->Failed) return nullptr;
    if (Regex_Peek(p) != ')') {
      Regex_SetErr(p, "unbalanced parenthesis");
      return nullptr;
    }
    p->Pos++;
    return inside;
  }
  if (c == '\\') {
    p->Pos++;
    RegexNode* escNode = nullptr;
    u8 lit = 0;
    if (!Regex_ParseEscape(p, &escNode, &lit)) return nullptr;
    return escNode ? escNode : Regex_NewLiteral(p, lit);
  }

  p->Pos++;
  return Regex_NewLiteral(p, c);
}

static RegexNode* Regex_ParseQuantified(RegexParser* p) {
  RegexNode* atom = Regex_ParseAtom(p);
  if (p->Failed) return nullptr;

  u32 min = 0;
  u32 max = 0;
  bool hasQuant = true;
  u8 c = Regex_Peek(p);
  if (c == '*') {
    p->Pos++;
    min = 0;
    max = REGEX_INF;
  } else if (c == '+') {
    p->Pos++;
    min = 1;
    max = REGEX_INF;
  } else if (c == '?') {
    p->Pos++;
    min = 0;
    max = 1;
  } else if (c == '{') {
    p->Pos++;
    if (!Regex_ParseNumber(p, &min)) {
      Regex_SetErr(p, "bad repeat bound");
      return nullptr;
    }
    if (Regex_Peek(p) == ',') {
      p->Pos++;
      if (Regex_IsDigit(Regex_Peek(p))) {
        if (!Regex_ParseNumber(p, &max)) {
          Regex_SetErr(p, "bad repeat bound");
          return nullptr;
        }
      } else {
        max = REGEX_INF;
      }
    } else {
      max = min;
    }
    if (Regex_Peek(p) != '}') {
      Regex_SetErr(p, "bad repeat bound");
      return nullptr;
    }
    p->Pos++;
    if (max != REGEX_INF && max < min) {
      Regex_SetErr(p, "bad repeat bound");
      return nullptr;
    }
  } else {
    hasQuant = false;
  }

  if (!hasQuant) return atom;

  bool lazy = false;
  if (Regex_Peek(p) == '?') {
    p->Pos++;
    lazy = true;
  }

  RegexNode* n = Regex_NewNode(p, RegexNode_Repeat);
  n->Repeat.Child = atom;
  n->Repeat.Min = min;
  n->Repeat.Max = max;
  n->Repeat.Lazy = lazy;
  return n;
}

static RegexNode* Regex_ParseConcat(RegexParser* p) {
  RegexNode** items = ArenaPushArray(p->Arena, RegexNode*, p->Pattern.Length + 1);
  u32 count = 0;

  while (!Regex_AtEnd(p)) {
    u8 c = Regex_Peek(p);
    if (c == '|' || c == ')') break;
    items[count++] = Regex_ParseQuantified(p);
    if (p->Failed) return nullptr;
  }

  if (count == 0) return Regex_NewEmpty(p);
  if (count == 1) return items[0];

  RegexNode* n = Regex_NewNode(p, RegexNode_Seq);
  n->Seq.Items = items;
  n->Seq.Count = count;
  return n;
}

static RegexNode* Regex_ParseExpression(RegexParser* p) {
  RegexNode* left = Regex_ParseConcat(p);
  if (p->Failed) return nullptr;

  while (Regex_Peek(p) == '|') {
    p->Pos++;
    RegexNode* right = Regex_ParseConcat(p);
    if (p->Failed) return nullptr;
    RegexNode* alt = Regex_NewNode(p, RegexNode_Alt);
    alt->Alt.Left = left;
    alt->Alt.Right = right;
    left = alt;
  }

  return left;
}

static bool Regex_Enter(RegexContext* ctx) {
  ctx->Steps++;
  if (ctx->Steps > ctx->StepLimit) return false;
  ctx->Depth++;
  if (ctx->Depth > ctx->DepthLimit) {
    ctx->Depth--;
    return false;
  }
  return true;
}

static void Regex_Leave(RegexContext* ctx) { ctx->Depth--; }

static bool Regex_ByteEq(const Regex* re, u8 a, u8 b) {
  if (re->CaseInsensitive) return Regex_Fold(a) == Regex_Fold(b);
  return a == b;
}

static bool Regex_RangeContains(const Regex* re, u8 c, u8 lo, u8 hi) {
  if (c >= lo && c <= hi) return true;
  if (!re->CaseInsensitive) return false;
  u8 fc = Regex_Fold(c);
  u8 flo = Regex_Fold(lo);
  u8 fhi = Regex_Fold(hi);
  if (flo <= fhi && fc >= flo && fc <= fhi) return true;
  return false;
}

static bool Regex_ClassAtomMatches(const Regex* re, const RegexClassAtom* a, u8 c) {
  switch (a->Kind) {
    case RegexClassAtom_Range: return Regex_RangeContains(re, c, a->Lo, a->Hi);
    case RegexClassAtom_Digit: return Regex_IsDigit(c);
    case RegexClassAtom_NotDigit: return !Regex_IsDigit(c);
    case RegexClassAtom_Word: return Regex_IsWord(c);
    case RegexClassAtom_NotWord: return !Regex_IsWord(c);
    case RegexClassAtom_Space: return Regex_IsSpace(c);
    case RegexClassAtom_NotSpace: return !Regex_IsSpace(c);
  }
  return false;
}

static bool Regex_ClassMatches(const Regex* re, const RegexClass* c, u8 ch) {
  bool matched = false;
  for (u32 i = 0; i < c->Count; i++) {
    if (Regex_ClassAtomMatches(re, c->Atoms + i, ch)) {
      matched = true;
      break;
    }
  }
  return c->Negate ? !matched : matched;
}

static bool Regex_MatchNode(RegexContext* ctx, const RegexNode* n, u32 pos, const RegexCont* cont, u32* end);
static bool Regex_MatchCont(RegexContext* ctx, const RegexCont* cont, u32 pos, u32* end);

static bool Regex_MatchRepeat(RegexContext* ctx, const RegexNode* n, u32 pos, u32 count, const RegexCont* after, u32* end) {
  if (!Regex_Enter(ctx)) return false;

  bool result = false;
  bool canStop = count >= n->Repeat.Min;
  bool canMore = count < n->Repeat.Max;

  if (n->Repeat.Lazy && canStop) result = Regex_MatchCont(ctx, after, pos, end);

  if (!result && canMore) {
    RegexCont rc;
    rc.Kind = RegexCont_Repeat;
    rc.Parent = after;
    rc.SeqNode = nullptr;
    rc.Index = 0;
    rc.RepeatNode = n;
    rc.Count = count + 1;
    rc.PrevPos = pos;
    result = Regex_MatchNode(ctx, n->Repeat.Child, pos, &rc, end);
  }

  if (!result && !n->Repeat.Lazy && canStop) result = Regex_MatchCont(ctx, after, pos, end);

  Regex_Leave(ctx);
  return result;
}

static bool Regex_MatchNode(RegexContext* ctx, const RegexNode* n, u32 pos, const RegexCont* cont, u32* end) {
  if (!Regex_Enter(ctx)) return false;

  bool result = false;
  switch (n->Kind) {
    case RegexNode_Empty:
      result = Regex_MatchCont(ctx, cont, pos, end);
      break;

    case RegexNode_Literal:
      if (pos < ctx->Text.Length && Regex_ByteEq(ctx->Re, (u8)ctx->Text.Data[pos], n->Literal)) {
        result = Regex_MatchCont(ctx, cont, pos + 1, end);
      }
      break;

    case RegexNode_Any:
      if (pos < ctx->Text.Length && ctx->Text.Data[pos] != '\n') {
        result = Regex_MatchCont(ctx, cont, pos + 1, end);
      }
      break;

    case RegexNode_Class:
      if (pos < ctx->Text.Length && Regex_ClassMatches(ctx->Re, n->Class, (u8)ctx->Text.Data[pos])) {
        result = Regex_MatchCont(ctx, cont, pos + 1, end);
      }
      break;

    case RegexNode_Start:
      if (pos == 0) result = Regex_MatchCont(ctx, cont, pos, end);
      break;

    case RegexNode_End:
      if (pos == ctx->Text.Length) result = Regex_MatchCont(ctx, cont, pos, end);
      break;

    case RegexNode_Seq: {
      RegexCont sc;
      sc.Kind = RegexCont_Seq;
      sc.Parent = cont;
      sc.SeqNode = n;
      sc.Index = 0;
      sc.RepeatNode = nullptr;
      sc.Count = 0;
      sc.PrevPos = pos;
      result = Regex_MatchCont(ctx, &sc, pos, end);
    } break;

    case RegexNode_Alt:
      result = Regex_MatchNode(ctx, n->Alt.Left, pos, cont, end);
      if (!result) result = Regex_MatchNode(ctx, n->Alt.Right, pos, cont, end);
      break;

    case RegexNode_Repeat:
      result = Regex_MatchRepeat(ctx, n, pos, 0, cont, end);
      break;
  }

  Regex_Leave(ctx);
  return result;
}

static bool Regex_MatchCont(RegexContext* ctx, const RegexCont* cont, u32 pos, u32* end) {
  if (!Regex_Enter(ctx)) return false;

  bool result = false;
  if (!cont || cont->Kind == RegexCont_Success) {
    *end = pos;
    result = true;
  } else if (cont->Kind == RegexCont_Seq) {
    if (cont->Index >= cont->SeqNode->Seq.Count) {
      result = Regex_MatchCont(ctx, cont->Parent, pos, end);
    } else {
      RegexCont next = *cont;
      next.Index++;
      result = Regex_MatchNode(ctx, cont->SeqNode->Seq.Items[cont->Index], pos, &next, end);
    }
  } else if (cont->Kind == RegexCont_Repeat) {
    if (pos == cont->PrevPos) {
      const RegexNode* r = cont->RepeatNode;
      if (cont->Count >= r->Repeat.Min) result = Regex_MatchCont(ctx, cont->Parent, pos, end);
    } else {
      result = Regex_MatchRepeat(ctx, cont->RepeatNode, pos, cont->Count, cont->Parent, end);
    }
  }

  Regex_Leave(ctx);
  return result;
}

Regex* Regex_Compile(Arena* arena, StrView pattern, bool caseInsensitive, StrBuf* err) {
  if (!arena) return nullptr;
  if (err) StrBuf_Clear(err);

  RegexParser p;
  p.Arena = arena;
  p.Pattern = pattern;
  p.Pos = 0;
  p.Err = err;
  p.Failed = false;

  RegexNode* root = Regex_ParseExpression(&p);
  if (p.Failed) return nullptr;
  if (p.Pos != pattern.Length) {
    Regex_SetErr(&p, p.Pattern.Data[p.Pos] == ')' ? "unbalanced parenthesis" : "parse error");
    return nullptr;
  }

  Regex* re = ArenaPushStruct(arena, Regex);
  re->Root = root;
  re->CaseInsensitive = caseInsensitive;
  re->PatternLength = pattern.Length;
  return re;
}

bool Regex_Search(const Regex* re, StrView text, u32* start, u32* len) {
  if (!re || !re->Root) return false;

  u32 limit = text.Length;
  for (u32 pos = 0; pos <= limit; pos++) {
    RegexContext ctx;
    ctx.Re = re;
    ctx.Text = text;
    ctx.Steps = 0;
    ctx.StepLimit =
        100000u + ClankMin(text.Length, 100000u) * 100u + ClankMin(re->PatternLength, 100000u) * 100u;
    ctx.Depth = 0;
    ctx.DepthLimit = 2048;

    RegexCont success;
    success.Kind = RegexCont_Success;
    success.Parent = nullptr;
    success.SeqNode = nullptr;
    success.Index = 0;
    success.RepeatNode = nullptr;
    success.Count = 0;
    success.PrevPos = pos;

    u32 end = pos;
    if (Regex_MatchNode(&ctx, re->Root, pos, &success, &end)) {
      if (start) *start = pos;
      if (len) *len = end - pos;
      return true;
    }
  }

  return false;
}
