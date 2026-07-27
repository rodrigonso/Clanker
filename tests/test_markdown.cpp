#include "base/arena.h"
#include "check.h"
#include "term/markdown.h"

namespace {

bool SpanHas(const Array<Span>* a, const char* needle, u8 attrMask, u8 fg) {
  for (u32 i = 0; i < a->Count; i++) {
    Span sp = a->Data[i];
    if (StrView_EqCStr(sp.Text, needle)) {
      if (attrMask && !(sp.St.Attr & attrMask)) return false;
      if (fg != 0 && sp.St.Fg != fg) return false;
      return true;
    }
  }
  return false;
}

}  // namespace

void TestMarkdown() {
  Arena a = ArenaCreate();
  Markdown_SetColorEnabled(true);

  // Inline: bold.
  {
    Array<Span> out = Array_Make<Span>(&a, 4);
    Markdown_Inline(SVL("a **b** c"), StyleDefault(), &out);
    CHECK_ALL(out.Count == 3, SpanHas(&out, "b", ATTR_BOLD, 0));
  }
  // Inline: code (green fg 114).
  {
    Array<Span> out = Array_Make<Span>(&a, 4);
    Markdown_Inline(SVL("x `code` y"), StyleDefault(), &out);
    CHECK(SpanHas(&out, "code", 0, 114));
  }
  // Inline: italic.
  {
    Array<Span> out = Array_Make<Span>(&a, 4);
    Markdown_Inline(SVL("_em_"), StyleDefault(), &out);
    CHECK(SpanHas(&out, "em", ATTR_ITALIC, 0));
  }
  // Inline: link text underlined, URL dropped.
  {
    Array<Span> out = Array_Make<Span>(&a, 4);
    Markdown_Inline(SVL("see [t](http://u) end"), StyleDefault(), &out);
    CHECK(SpanHas(&out, "t", ATTR_UNDERLINE, 0));
  }

  // Block: heading.
  {
    Array<SpanLine> out = Array_Make<SpanLine>(&a, 4);
    Markdown_Render(&a, SVL("# Title"), &out);
    CHECK_ALL(out.Count == 1,
              SpanHas(&out.Data[0].Spans, "Title", ATTR_BOLD, 0));
  }
  // Block: unordered list bullet.
  {
    Array<SpanLine> out = Array_Make<SpanLine>(&a, 4);
    Markdown_Render(&a, SVL("- item"), &out);
    CHECK_ALL(out.Count == 1,
              SpanHas(&out.Data[0].Spans, "\xe2\x80\xa2 ", 0, 0),
              SpanHas(&out.Data[0].Spans, "item", 0, 0));
  }
  // Block: fenced code -> code style, fence lines dropped.
  {
    Array<SpanLine> out = Array_Make<SpanLine>(&a, 4);
    Markdown_Render(&a, SVL("```\ncode\n```"), &out);
    CHECK_ALL(out.Count == 1, SpanHas(&out.Data[0].Spans, "code", 0, 114));
  }

  ArenaRelease(&a);
}
