#pragma once
#include "tui/linemodel.h"

// Markdown -> styled logical span-lines (block structure resolved, inline
// styling applied, long lines left for the line model to wrap).
//
// Modelled on pi's packages/tui/src/components/markdown.ts, which lexes with
// `marked` and hand-writes the walk to ANSI. Clank keeps the same visual
// vocabulary but scans lines directly instead of materialising a token tree:
// Markdown_Render runs on every streamed delta (LineModel_AppendAssistant
// re-renders the whole assistant entry), so allocation stays proportional to
// output. Only blockquotes, list items and table cells recurse, rendering into
// a temporary row array in the caller's arena that is then abandoned; the
// waste is bounded by the nested block's own size.
//
// Deliberate divergences from pi, all documented in docs/UI.md:
//   - No OSC 8 hyperlinks. Screen stores one scalar per cell and re-emits it
//     verbatim, so an escape sequence cannot live in the grid; links always
//     use pi's non-hyperlink fallback, `text (url)`.
//   - No syntax highlighting inside fences (pi's own behaviour for an unknown
//     language tag) and no LaTeX.
//   - No GFM column alignment, which pi does not implement either.
//   - A list marker wider than 4 columns (`- [x] `, `100. `) keeps its nested
//     list under the parent's content instead of pulling it back to pi's
//     absolute 4-column grid.

// 256-colour palette indices, chosen as the nearest xterm cube/ramp entries to
// pi's builtin dark theme. Clank's Style carries a palette index rather than
// 24-bit colour, so these are approximations.
struct MarkdownPalette {
  u8 Heading;          // pi mdHeading #f0c674
  u8 Code;             // pi mdCode #8abeb7 (inline code span)
  u8 Link;             // pi mdLink #81a2be
  u8 LinkUrl;          // pi mdLinkUrl #666666
  u8 CodeBlock;        // pi mdCodeBlock #b5bd68 (fence body)
  u8 CodeBlockBorder;  // pi mdCodeBlockBorder
  u8 Quote;            // pi mdQuote
  u8 QuoteBorder;      // pi mdQuoteBorder
  u8 Hr;               // pi mdHr
  u8 ListBullet;       // pi mdListBullet
  u8 TableBorder;
};

const MarkdownPalette& Markdown_DefaultPalette();

struct MarkdownOpts {
  // Content columns available to the renderer. Horizontal rules and tables are
  // sized against this, so it must be passed per render rather than cached:
  // LineModel_FitsAtWidth re-renders at a trial width.
  u16 Width;
  bool Color;
};

inline MarkdownOpts MarkdownOpts_Make(u16 width, bool color) {
  MarkdownOpts opts;
  opts.Width = width ? width : 80;
  opts.Color = color;
  return opts;
}

// Blocks: ATX and setext headings, fenced (``` and ~~~) and indented code,
// bullet/ordered/task lists with nesting, blockquotes, horizontal rules, GFM
// tables, HTML passthrough, and paragraphs.
//
// Rows carry their own wrap continuation prefix (list marker width, blockquote
// bar, code indent) in SpanLine::Continuation. Callers that add an outer
// prefix must concatenate onto it rather than overwrite it.
void Markdown_Render(Arena* a, StrView md, Array<SpanLine>* out, MarkdownOpts opts);

// Inline-only render of one line (no block handling); exposed for tests/reuse.
// Handles backslash escapes, code spans, emphasis, strikethrough, links,
// images and autolinks. `a` backs the few spans that are not substrings of
// `line` (the " (url)" link suffix).
void Markdown_Inline(Arena* a, StrView line, Style base, Array<Span>* out,
                     bool color = true);
