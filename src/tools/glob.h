#pragma once
#include "base/str.h"

// Match `path` against glob `pattern`. `path` uses '/' separators.
// Supports: '*' (any run of non-'/' chars), '?' (one non-'/' char),
// '**' (any chars including '/'), '[...]' char class (with ranges a-z and
// leading '!' or '^' negation), and '{a,b,c}' brace alternation (no nesting).
// `caseInsensitive` lowercases ASCII on both sides for comparison.
bool Glob_Match(StrView pattern, StrView path, bool caseInsensitive);
