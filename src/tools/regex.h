#pragma once
#include "base/arena.h"
#include "base/str.h"

// Small ASCII regex engine for grep-style line matching.
//
// Supported syntax:
// - Literal characters; '.' matches any byte except '\n'.
// - '^' and '$' anchor to the start/end of the supplied text.
// - Quantifiers '*', '+', '?', '{m}', '{m,}', '{m,n}', and lazy '*?', '+?',
//   '??', '{...}?' variants.
// - Character classes with ranges and negation: '[a-z]', '[^0-9]'.
// - Escapes: '\d'/'\D', '\w'/'\W', '\s'/'\S', '\n', '\t', '\r', and escaped
//   metacharacters such as '\.', '\*', '\\'.
// - Grouping with '(' and ')' and alternation with '|'. Captures are not
//   reported; groups are only for precedence and quantifiers.
//
// Matching is byte-oriented with ASCII case folding when requested.

struct Regex;  // opaque, arena-allocated

// Compile `pattern`. Returns nullptr on error and writes a message into `err` (if non-null).
Regex* Regex_Compile(Arena* arena, StrView pattern, bool caseInsensitive, StrBuf* err);

// Find the FIRST (leftmost) match within `text`. On success returns true and sets
// *start and *len to the byte offset and length of the match. (*start/*len may be null.)
bool Regex_Search(const Regex* re, StrView text, u32* start, u32* len);

// Convenience: does `text` contain a match anywhere?
inline bool Regex_Test(const Regex* re, StrView text) { return Regex_Search(re, text, nullptr, nullptr); }
