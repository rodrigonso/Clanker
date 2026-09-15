#pragma once
#include "base/str.h"

// Match `path` against glob `pattern`. `path` uses '/' separators.
// Supports: '*' (any run of non-'/' chars), '?' (one non-'/' char),
// '**' (any chars including '/'), '[...]' char class (with ranges a-z and
// leading '!' or '^' negation), and '{a,b,c}' brace alternation (no nesting).
// `caseInsensitive` lowercases ASCII on both sides for comparison.
// Backslash quotes the next character; '**/' also matches zero directories.
bool Glob_Match(StrView pattern, StrView path, bool caseInsensitive);
// Returns false when matching exceeds 262144 work steps, 64 recursive levels,
// a 16384-byte pattern, or a 32768-byte path.
bool Glob_MatchChecked(StrView pattern, StrView path, bool caseInsensitive,
                       bool* matched);
// Strict syntax validation for user-supplied patterns. Ignore-file matching
// retains permissive literal handling for unmatched brackets and braces.
bool Glob_Validate(StrView pattern, StrBuf* err);
