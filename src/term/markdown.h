#pragma once
#include "term/linemodel.h"

// Basic markdown -> styled logical span-lines (unwrapped). Supports headings,
// bold/italic, inline code, fenced code blocks, ordered/unordered lists,
// blockquotes, and links. Colours are emitted unless disabled.
void Markdown_SetColorEnabled(bool on);
void Markdown_Render(Arena* a, StrView md, Array<SpanLine>* out);

// Inline-only render of one line (no block handling); exposed for tests/reuse.
void Markdown_Inline(StrView line, Style base, Array<Span>* out);
