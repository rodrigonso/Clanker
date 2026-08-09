#pragma once
#include "base/arena.h"
#include "base/str.h"

struct FrontmatterField {
  StrView Key;
  StrView Value;
};

// Resolves requested top-level scalars and the body from YAML frontmatter.
// Returned views may point into `arena` after CRLF normalization.
void Frontmatter_Parse(Arena* arena, StrView content, FrontmatterField* fields,
                       u32 fieldCount, StrView* body);

bool Frontmatter_ParseBool(StrView value);
