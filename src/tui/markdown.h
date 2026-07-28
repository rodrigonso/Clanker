#pragma once
#include "tui/linemodel.h"

// Basic markdown -> styled logical span-lines (unwrapped). Supports headings,
// bold/italic, inline code, fenced code blocks, ordered/unordered lists,
// blockquotes, and links. Colours are optional per render.
void Markdown_Render(Arena* a, StrView md, Array<SpanLine>* out,
                     bool color = true);

// Inline-only render of one line (no block handling); exposed for tests/reuse.
void Markdown_Inline(StrView line, Style base, Array<Span>* out,
                     bool color = true);
