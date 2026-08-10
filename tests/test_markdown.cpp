#include "base/arena.h"
#include "check.h"
#include "tui/markdown.h"

namespace {

const MarkdownPalette& Pal() { return Markdown_DefaultPalette(); }

bool SpanHas(const Array<Span>* a, const char* needle, u8 attrMask, u8 fg) {
  for (u32 i = 0; i < a->Count; i++) {
    Span sp = a->Data[i];
    if (!StrView_EqCStr(sp.Text, needle)) continue;
    if (attrMask && (sp.St.Attr & attrMask) != attrMask) continue;
    if (fg != 0 && sp.St.Fg != fg) continue;
    return true;
  }
  return false;
}

// Flatten one rendered row to plain text so structural assertions stay
// readable. The continuation prefix is excluded: it only appears on wrapped
// rows, which the line model produces, not the markdown renderer.
StrView RowText(Arena* a, const SpanLine* line) {
  StrBuf out = StrBuf_Make(a, 64);
  for (u32 i = 0; i < line->Spans.Count; i++) StrBuf_Append(&out, line->Spans.Data[i].Text);
  return StrBuf_View(&out);
}

bool RowIs(Arena* a, const Array<SpanLine>* rows, u32 index, const char* expected) {
  if (index >= rows->Count) return false;
  return StrView_EqCStr(RowText(a, &rows->Data[index]), expected);
}

bool AnyRowIs(Arena* a, const Array<SpanLine>* rows, const char* expected) {
  for (u32 i = 0; i < rows->Count; i++)
    if (RowIs(a, rows, i, expected)) return true;
  return false;
}

// Style of the first span whose text matches exactly.
Style StyleOf(const Array<SpanLine>* rows, const char* needle, bool* found) {
  for (u32 r = 0; r < rows->Count; r++) {
    const Array<Span>* spans = &rows->Data[r].Spans;
    for (u32 i = 0; i < spans->Count; i++) {
      if (StrView_EqCStr(spans->Data[i].Text, needle)) {
        *found = true;
        return spans->Data[i].St;
      }
    }
  }
  *found = false;
  return StyleDefault();
}

Array<SpanLine> Render(Arena* a, const char* md, u16 width = 80) {
  Array<SpanLine> out = Array_Make<SpanLine>(a, 8);
  Markdown_Render(a, SV(md), &out, MarkdownOpts_Make(width, true));
  return out;
}

void TestInline(Arena* a) {
  {
    Array<Span> out = Array_Make<Span>(a, 4);
    Markdown_Inline(a, SVL("a **b** c"), StyleDefault(), &out);
    CHECK(SpanHas(&out, "b", ATTR_BOLD, 0));
  }
  {
    Array<Span> out = Array_Make<Span>(a, 4);
    Markdown_Inline(a, SVL("x `code` y"), StyleDefault(), &out);
    CHECK(SpanHas(&out, "code", 0, Pal().Code));
  }
  {
    Array<Span> out = Array_Make<Span>(a, 4);
    Markdown_Inline(a, SVL("_em_"), StyleDefault(), &out);
    CHECK(SpanHas(&out, "em", ATTR_ITALIC, 0));
  }
  // Bold+italic: `***x***` must not leak a stray asterisk.
  {
    Array<Span> out = Array_Make<Span>(a, 4);
    Markdown_Inline(a, SVL("***x***"), StyleDefault(), &out);
    CHECK_ALL(SpanHas(&out, "x", (u8)(ATTR_BOLD | ATTR_ITALIC), 0),
              !SpanHas(&out, "*x", 0, 0));
  }
  // Intraword underscores are not emphasis.
  {
    Array<Span> out = Array_Make<Span>(a, 4);
    Markdown_Inline(a, SVL("snake_case_name"), StyleDefault(), &out);
    for (u32 i = 0; i < out.Count; i++) CHECK((out.Data[i].St.Attr & ATTR_ITALIC) == 0);
  }
  // Strikethrough, with pi's strict spacing rule.
  {
    Array<Span> out = Array_Make<Span>(a, 4);
    Markdown_Inline(a, SVL("~~gone~~"), StyleDefault(), &out);
    CHECK(SpanHas(&out, "gone", ATTR_STRIKE, 0));
  }
  {
    Array<Span> out = Array_Make<Span>(a, 4);
    Markdown_Inline(a, SVL("~~ spaced ~~"), StyleDefault(), &out);
    for (u32 i = 0; i < out.Count; i++) CHECK((out.Data[i].St.Attr & ATTR_STRIKE) == 0);
  }
  // Backslash escapes drop the backslash and suppress the markup.
  {
    Array<Span> out = Array_Make<Span>(a, 4);
    Markdown_Inline(a, SVL("\\*not bold\\*"), StyleDefault(), &out);
    StrBuf flat = StrBuf_Make(a, 32);
    for (u32 i = 0; i < out.Count; i++) {
      StrBuf_Append(&flat, out.Data[i].Text);
      CHECK((out.Data[i].St.Attr & ATTR_BOLD) == 0);
    }
    CHECK(StrView_EqCStr(StrBuf_View(&flat), "*not bold*"));
  }
  // Link: text underlined in link colour, URL kept via pi's " (url)" fallback.
  {
    Array<Span> out = Array_Make<Span>(a, 4);
    Markdown_Inline(a, SVL("see [t](http://u) end"), StyleDefault(), &out);
    CHECK_ALL(SpanHas(&out, "t", ATTR_UNDERLINE, Pal().Link),
              SpanHas(&out, " (http://u)", 0, Pal().LinkUrl));
  }
  // A bare URL is its own label, so no duplicate suffix.
  {
    Array<Span> out = Array_Make<Span>(a, 4);
    Markdown_Inline(a, SVL("go https://x.dev/a now"), StyleDefault(), &out);
    CHECK_ALL(SpanHas(&out, "https://x.dev/a", ATTR_UNDERLINE, Pal().Link),
              !SpanHas(&out, " (https://x.dev/a)", 0, 0));
  }
  // Trailing sentence punctuation is not part of the URL.
  {
    Array<Span> out = Array_Make<Span>(a, 4);
    Markdown_Inline(a, SVL("see https://x.dev."), StyleDefault(), &out);
    CHECK(SpanHas(&out, "https://x.dev", ATTR_UNDERLINE, Pal().Link));
  }
  {
    Array<Span> out = Array_Make<Span>(a, 4);
    Markdown_Inline(a, SVL("<https://x.dev>"), StyleDefault(), &out);
    CHECK(SpanHas(&out, "https://x.dev", ATTR_UNDERLINE, Pal().Link));
  }
  // Images render alt text only, with no stray "!".
  {
    Array<Span> out = Array_Make<Span>(a, 4);
    Markdown_Inline(a, SVL("![alt](u.png)"), StyleDefault(), &out);
    StrBuf flat = StrBuf_Make(a, 32);
    for (u32 i = 0; i < out.Count; i++) StrBuf_Append(&flat, out.Data[i].Text);
    CHECK(StrView_EqCStr(StrBuf_View(&flat), "alt"));
  }
  // Code spans are not re-parsed for emphasis.
  {
    Array<Span> out = Array_Make<Span>(a, 4);
    Markdown_Inline(a, SVL("`a *b* c`"), StyleDefault(), &out);
    CHECK(SpanHas(&out, "a *b* c", 0, Pal().Code));
  }
  // Colour can be suppressed while attributes survive.
  {
    Array<Span> out = Array_Make<Span>(a, 4);
    Markdown_Inline(a, SVL("`plain`"), StyleDefault(), &out, false);
    CHECK_ALL(out.Count == 1, out.Data[0].St.Fg == COL_DEFAULT);
  }
  // An unmatched delimiter stays literal.
  {
    Array<Span> out = Array_Make<Span>(a, 4);
    Markdown_Inline(a, SVL("2 * 3 = 6"), StyleDefault(), &out);
    StrBuf flat = StrBuf_Make(a, 32);
    for (u32 i = 0; i < out.Count; i++) StrBuf_Append(&flat, out.Data[i].Text);
    CHECK(StrView_EqCStr(StrBuf_View(&flat), "2 * 3 = 6"));
  }
}

void TestHeadings(Arena* a) {
  // h1 keeps no marker and is underlined; h2 is bold only.
  {
    Array<SpanLine> out = Render(a, "# Title");
    CHECK_ALL(out.Count == 1, RowIs(a, &out, 0, "Title"),
              SpanHas(&out.Data[0].Spans, "Title", (u8)(ATTR_BOLD | ATTR_UNDERLINE),
                      Pal().Heading));
  }
  {
    Array<SpanLine> out = Render(a, "## Two");
    bool found = false;
    Style st = StyleOf(&out, "Two", &found);
    CHECK_ALL(found, (st.Attr & ATTR_BOLD) != 0, (st.Attr & ATTR_UNDERLINE) == 0);
  }
  // h3+ keep a styled marker.
  {
    Array<SpanLine> out = Render(a, "### Three");
    CHECK(RowIs(a, &out, 0, "### Three"));
  }
  // A closing run of hashes is decoration.
  {
    Array<SpanLine> out = Render(a, "## Two ##");
    CHECK(RowIs(a, &out, 0, "Two"));
  }
  // Setext underlines.
  {
    Array<SpanLine> out = Render(a, "Title\n=====");
    bool found = false;
    Style st = StyleOf(&out, "Title", &found);
    CHECK_ALL(out.Count == 1, found, (st.Attr & ATTR_UNDERLINE) != 0);
  }
  {
    Array<SpanLine> out = Render(a, "Sub\n---");
    bool found = false;
    Style st = StyleOf(&out, "Sub", &found);
    CHECK_ALL(out.Count == 1, found, (st.Attr & ATTR_BOLD) != 0,
              (st.Attr & ATTR_UNDERLINE) == 0);
  }
  // A blank row separates a heading from the body that follows it.
  {
    Array<SpanLine> out = Render(a, "# H\n\nbody");
    CHECK_ALL(out.Count == 3, RowIs(a, &out, 0, "H"), RowIs(a, &out, 1, ""),
              RowIs(a, &out, 2, "body"));
  }
  // Inline markup applies inside a heading.
  {
    Array<SpanLine> out = Render(a, "# A `b` C");
    CHECK(SpanHas(&out.Data[0].Spans, "b", 0, Pal().Code));
  }
}

void TestCodeBlocks(Arena* a) {
  // Fences are visible and the body is indented two columns, as in pi.
  {
    Array<SpanLine> out = Render(a, "```\ncode\n```");
    CHECK_ALL(out.Count == 3, RowIs(a, &out, 0, "```"), RowIs(a, &out, 1, "  code"),
              RowIs(a, &out, 2, "```"));
    CHECK(SpanHas(&out.Data[1].Spans, "code", 0, Pal().CodeBlock));
  }
  // The info string rides on the opening fence.
  {
    Array<SpanLine> out = Render(a, "```rust\nfn main() {}\n```");
    CHECK_ALL(RowIs(a, &out, 0, "```rust"), RowIs(a, &out, 1, "  fn main() {}"));
  }
  // Tilde fences, with backticks as ordinary content inside them.
  {
    Array<SpanLine> out = Render(a, "~~~\n```\n~~~");
    CHECK_ALL(out.Count == 3, RowIs(a, &out, 1, "  ```"));
  }
  // Markdown inside a fence is never interpreted.
  {
    Array<SpanLine> out = Render(a, "```\n# not a heading\n```");
    CHECK(RowIs(a, &out, 1, "  # not a heading"));
  }
  {
    Array<SpanLine> out = Render(a, "    indented");
    CHECK_ALL(out.Count == 3, RowIs(a, &out, 0, "```"), RowIs(a, &out, 1, "  indented"));
  }
  // Fence body rows carry the indent as their wrap continuation.
  {
    Array<SpanLine> out = Render(a, "```\nx\n```");
    CHECK_ALL(out.Data[1].HasContinuation,
              StrView_EqCStr(out.Data[1].Continuation.Text, "  "));
  }
}

void TestLists(Arena* a) {
  // Every bullet character normalises to "- ".
  {
    Array<SpanLine> out = Render(a, "* item");
    CHECK_ALL(out.Count == 1, RowIs(a, &out, 0, "- item"));
    CHECK(SpanHas(&out.Data[0].Spans, "- ", 0, Pal().ListBullet));
  }
  // Ordered lists renumber from the first item's value.
  {
    Array<SpanLine> out = Render(a, "3. a\n4. b");
    CHECK_ALL(out.Count == 2, RowIs(a, &out, 0, "3. a"), RowIs(a, &out, 1, "4. b"));
  }
  // pi puts nested lists on an absolute 4-column grid, not under the parent's
  // content, so the child's own marker starts at column 4.
  {
    Array<SpanLine> out = Render(a, "- top\n    - nested");
    CHECK_ALL(out.Count == 2, RowIs(a, &out, 0, "- top"),
              RowIs(a, &out, 1, "    - nested"));
  }
  {
    Array<SpanLine> out = Render(a, "- a\n    - b\n        - c");
    CHECK_ALL(out.Count == 3, RowIs(a, &out, 1, "    - b"),
              RowIs(a, &out, 2, "        - c"));
  }
  // An ordered parent lands its child on the same grid.
  {
    Array<SpanLine> out = Render(a, "1. top\n   - nested");
    CHECK(RowIs(a, &out, 1, "    - nested"));
  }
  {
    Array<SpanLine> out = Render(a, "- [x] done\n- [ ] todo");
    CHECK_ALL(RowIs(a, &out, 0, "- [x] done"), RowIs(a, &out, 1, "- [ ] todo"));
  }
  // Wrapped item text aligns under the content, not the margin.
  {
    Array<SpanLine> out = Render(a, "- item");
    CHECK_ALL(out.Data[0].HasContinuation,
              StrView_EqCStr(out.Data[0].Continuation.Text, "  "));
  }
  {
    Array<SpanLine> out = Render(a, "10. item");
    CHECK(StrView_EqCStr(out.Data[0].Continuation.Text, "    "));
  }
  {
    Array<SpanLine> out = Render(a, "- [x] done");
    CHECK(StrView_EqCStr(out.Data[0].Continuation.Text, "      "));
  }
  // A loose list keeps one blank row between items.
  {
    Array<SpanLine> out = Render(a, "- a\n\n- b");
    CHECK_ALL(out.Count == 3, RowIs(a, &out, 0, "- a"), RowIs(a, &out, 1, ""),
              RowIs(a, &out, 2, "- b"));
  }
  // A tight list has none.
  {
    Array<SpanLine> out = Render(a, "- a\n- b");
    CHECK(out.Count == 2);
  }
  {
    Array<SpanLine> out = Render(a, "- **bold** item");
    CHECK(SpanHas(&out.Data[0].Spans, "bold", ATTR_BOLD, 0));
  }
  // A fence nested in an item keeps the item's indent.
  {
    Array<SpanLine> out = Render(a, "- text\n\n      code");
    CHECK(AnyRowIs(a, &out, "  ```"));
  }
  // A soft break inside an item is a real break, aligned under the content.
  {
    Array<SpanLine> out = Render(a, "- one\n  two");
    CHECK_ALL(out.Count == 2, RowIs(a, &out, 0, "- one"), RowIs(a, &out, 1, "  two"));
  }
}

void TestQuotes(Arena* a) {
  {
    Array<SpanLine> out = Render(a, "> quoted");
    CHECK_ALL(out.Count == 1, RowIs(a, &out, 0, "\xe2\x94\x82 quoted"));
    bool found = false;
    Style st = StyleOf(&out, "quoted", &found);
    CHECK_ALL(found, (st.Attr & ATTR_ITALIC) != 0, st.Fg == Pal().Quote);
    CHECK(SpanHas(&out.Data[0].Spans, "\xe2\x94\x82 ", 0, Pal().QuoteBorder));
  }
  // The bar is re-applied on wrapped rows too.
  {
    Array<SpanLine> out = Render(a, "> quoted");
    CHECK_ALL(out.Data[0].HasContinuation,
              StrView_EqCStr(out.Data[0].Continuation.Text, "\xe2\x94\x82 "));
  }
  // Nesting stacks the bars.
  {
    Array<SpanLine> out = Render(a, "> > deep");
    CHECK(RowIs(a, &out, 0, "\xe2\x94\x82 \xe2\x94\x82 deep"));
  }
  // Lazy continuation pulls the following plain line into the quote, keeping
  // it on its own row.
  {
    Array<SpanLine> out = Render(a, "> one\ntwo");
    CHECK_ALL(out.Count == 2, RowIs(a, &out, 0, "\xe2\x94\x82 one"),
              RowIs(a, &out, 1, "\xe2\x94\x82 two"));
  }
  // A list inside a quote keeps both prefixes.
  {
    Array<SpanLine> out = Render(a, "> - item");
    CHECK(RowIs(a, &out, 0, "\xe2\x94\x82 - item"));
  }
  // The quote's italic survives an inline code span inside it.
  {
    Array<SpanLine> out = Render(a, "> a `b` c");
    CHECK(SpanHas(&out.Data[0].Spans, "b", ATTR_ITALIC, Pal().Code));
  }
}

void TestRules(Arena* a) {
  {
    Array<SpanLine> out = Render(a, "---", 10);
    CHECK_ALL(out.Count == 1, TextWidth(RowText(a, &out.Data[0])) == 10,
              out.Data[0].Spans.Data[0].St.Fg == Pal().Hr);
  }
  // Capped at 80 columns like pi.
  {
    Array<SpanLine> out = Render(a, "***", 200);
    CHECK(TextWidth(RowText(a, &out.Data[0])) == 80);
  }
  // `***` and `___` are rules, not emphasis.
  {
    Array<SpanLine> out = Render(a, "___", 8);
    CHECK(TextWidth(RowText(a, &out.Data[0])) == 8);
  }
  // A rule between paragraphs still separates them.
  {
    Array<SpanLine> out = Render(a, "a\n\n---\n\nb");
    CHECK_ALL(out.Count == 5, RowIs(a, &out, 0, "a"), RowIs(a, &out, 4, "b"));
  }
}

void TestTables(Arena* a) {
  const char* md = "| a | b |\n|---|---|\n| 1 | 2 |";
  {
    Array<SpanLine> out = Render(a, md);
    // top rule, header, separator, one data row, bottom rule
    CHECK(out.Count == 5);
    CHECK_ALL(RowIs(a, &out, 1, "\xe2\x94\x82 a \xe2\x94\x82 b \xe2\x94\x82"),
              RowIs(a, &out, 3, "\xe2\x94\x82 1 \xe2\x94\x82 2 \xe2\x94\x82"));
  }
  // Header cells are bold.
  {
    Array<SpanLine> out = Render(a, md);
    CHECK(SpanHas(&out.Data[1].Spans, "a", ATTR_BOLD, 0));
  }
  // Every emitted row is exactly the table's width.
  {
    Array<SpanLine> out = Render(a, md);
    u32 width = TextWidth(RowText(a, &out.Data[0]));
    CHECK(width == 9);
    for (u32 i = 0; i < out.Count; i++) CHECK(TextWidth(RowText(a, &out.Data[i])) == width);
  }
  // Alignment markers are accepted (and, as in pi, ignored).
  {
    Array<SpanLine> out = Render(a, "| a | b |\n|:--|--:|\n| 1 | 2 |");
    CHECK(out.Count == 5);
  }
  // Ragged rows are padded out rather than truncating the frame.
  {
    Array<SpanLine> out = Render(a, "| a | b |\n|---|---|\n| 1 |");
    u32 width = TextWidth(RowText(a, &out.Data[0]));
    for (u32 i = 0; i < out.Count; i++) CHECK(TextWidth(RowText(a, &out.Data[i])) == width);
  }
  // Wide content shrinks to the available width rather than overflowing.
  {
    Array<SpanLine> out =
        Render(a, "| alpha beta | gamma delta |\n|---|---|\n| one two | three four |", 24);
    for (u32 i = 0; i < out.Count; i++) CHECK(TextWidth(RowText(a, &out.Data[i])) <= 24);
  }
  // Too narrow for a table: fall back to the raw source.
  {
    Array<SpanLine> out = Render(a, md, 4);
    CHECK(AnyRowIs(a, &out, "| a | b |"));
  }
  // A pipe line with no delimiter row stays a paragraph.
  {
    Array<SpanLine> out = Render(a, "a | b");
    CHECK_ALL(out.Count == 1, RowIs(a, &out, 0, "a | b"));
  }
  // Inline markup applies inside cells.
  {
    Array<SpanLine> out = Render(a, "| a | b |\n|---|---|\n| `x` | y |");
    CHECK(SpanHas(&out.Data[3].Spans, "x", 0, Pal().Code));
  }
}

void TestParagraphs(Arena* a) {
  // A soft break stays a break, as in pi: agent replies use single newlines
  // deliberately, so reflowing them would move text the model laid out.
  {
    Array<SpanLine> out = Render(a, "one\ntwo");
    CHECK_ALL(out.Count == 2, RowIs(a, &out, 0, "one"), RowIs(a, &out, 1, "two"));
  }
  // Emphasis still spans a soft break, because the inline pass sees the whole
  // paragraph before it is cut into rows.
  {
    Array<SpanLine> out = Render(a, "**bold\nacross**");
    CHECK_ALL(out.Count == 2, SpanHas(&out.Data[0].Spans, "bold", ATTR_BOLD, 0),
              SpanHas(&out.Data[1].Spans, "across", ATTR_BOLD, 0));
  }
  // Two trailing spaces are a hard break.
  {
    Array<SpanLine> out = Render(a, "one  \ntwo");
    CHECK_ALL(out.Count == 2, RowIs(a, &out, 0, "one"), RowIs(a, &out, 1, "two"));
  }
  // Exactly one blank row survives between blocks...
  {
    Array<SpanLine> out = Render(a, "a\n\n\n\nb");
    CHECK_ALL(out.Count == 3, RowIs(a, &out, 1, ""));
  }
  // ...and none is left dangling at either end, which would make a streaming
  // reply jump as it grows.
  {
    Array<SpanLine> out = Render(a, "\n\na\n\n");
    CHECK_ALL(out.Count == 1, RowIs(a, &out, 0, "a"));
  }
  // A list directly under a paragraph is not separated, matching pi.
  {
    Array<SpanLine> out = Render(a, "intro\n- item");
    CHECK_ALL(out.Count == 2, RowIs(a, &out, 0, "intro"), RowIs(a, &out, 1, "- item"));
  }
  // HTML passes through as text.
  {
    Array<SpanLine> out = Render(a, "<div>raw</div>");
    CHECK(RowIs(a, &out, 0, "<div>raw</div>"));
  }
  // Tabs normalise so they cannot desync the cell grid.
  {
    Array<SpanLine> out = Render(a, "a\tb");
    CHECK(RowIs(a, &out, 0, "a   b"));
  }
  // CRLF input is handled without leaving stray carriage returns.
  {
    Array<SpanLine> out = Render(a, "a\r\n\r\nb");
    CHECK_ALL(out.Count == 3, RowIs(a, &out, 0, "a"), RowIs(a, &out, 2, "b"));
  }
  // Empty input renders nothing at all.
  {
    Array<SpanLine> out = Render(a, "");
    CHECK(out.Count == 0);
  }
}

void TestStreaming(Arena* a) {
  // A closing fence typed one backtick at a time must not resize the block.
  Array<SpanLine> closed = Render(a, "```\ncode\n```");
  {
    Array<SpanLine> out = Render(a, "```\ncode\n");
    CHECK_ALL(out.Count == closed.Count, RowIs(a, &out, 1, "  code"));
  }
  {
    Array<SpanLine> out = Render(a, "```\ncode\n`");
    CHECK_ALL(out.Count == closed.Count, RowIs(a, &out, 1, "  code"));
  }
  {
    Array<SpanLine> out = Render(a, "```\ncode\n``");
    CHECK_ALL(out.Count == closed.Count, RowIs(a, &out, 1, "  code"));
  }
  // A complete fence is untouched by the trim.
  {
    Array<SpanLine> out = Render(a, "```\na\n```\n\nafter");
    CHECK(AnyRowIs(a, &out, "after"));
  }
  // Backticks that are ordinary content are not mistaken for a partial fence.
  {
    Array<SpanLine> out = Render(a, "text ``");
    CHECK(RowIs(a, &out, 0, "text ``"));
  }
  // Growing text keeps a stable prefix: the rows rendered for a prefix of the
  // message stay a prefix of the rows rendered for the whole message.
  {
    Array<SpanLine> whole = Render(a, "# Title\n\nSome text\n\n- one\n- two\n");
    Array<SpanLine> head = Render(a, "# Title\n\nSome text\n\n- one");
    CHECK(head.Count <= whole.Count);
    for (u32 i = 0; i < head.Count && i < whole.Count; i++)
      CHECK(StrView_Eq(RowText(a, &head.Data[i]), RowText(a, &whole.Data[i])));
  }
}

}  // namespace

void TestMarkdown() {
  Arena a = ArenaCreate();
  TestInline(&a);
  TestHeadings(&a);
  TestCodeBlocks(&a);
  TestLists(&a);
  TestQuotes(&a);
  TestRules(&a);
  TestTables(&a);
  TestParagraphs(&a);
  TestStreaming(&a);
  ArenaRelease(&a);
}
