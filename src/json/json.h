#pragma once
#include "base/arena.h"
#include "base/basics.h"
#include "base/str.h"

// Minimal arena-based JSON. The parser builds a small DOM; the writer provides
// string escaping for hand-built request bodies. Input to Json_Parse must be
// null-terminated at [Length] (StrBuf guarantees this).

enum class JsonKind : u8 { Null, Bool, Number, String, Array, Object };

struct JsonValue;

struct JsonMember {
  StrView Key;
  JsonValue* Value;
};

struct JsonValue {
  JsonKind Kind;
  bool Bool;
  f64 Number;
  StrView Str;  // decoded (unescaped) string, arena-owned
  JsonValue** Items;
  u32 Count;
  JsonMember* Members;
  u32 MemberCount;
};

// Returns nullptr on parse error.
JsonValue* Json_Parse(Arena* arena, StrView text);
JsonValue* Json_Clone(Arena* arena, const JsonValue* value);
bool Json_Equals(const JsonValue* a, const JsonValue* b);

JsonValue* Json_Get(const JsonValue* obj, const char* key);
StrView Json_GetStr(const JsonValue* obj, const char* key);  // "" if absent/not a string
f64 Json_GetNum(const JsonValue* obj, const char* key, f64 fallback);
i64 Json_GetInt(const JsonValue* obj, const char* key, i64 fallback);
bool Json_GetBool(const JsonValue* obj, const char* key, bool fallback);

inline bool Json_IsObject(const JsonValue* v) { return v && v->Kind == JsonKind::Object; }
inline bool Json_IsArray(const JsonValue* v) { return v && v->Kind == JsonKind::Array; }
inline bool Json_IsString(const JsonValue* v) { return v && v->Kind == JsonKind::String; }

// Append `s` to `b` as a JSON-escaped, double-quoted string literal.
u32 Json_FormatNumber(char* dst, u32 cap, f64 value);
void Json_WriteString(StrBuf* b, StrView s);
void Json_WriteValue(StrBuf* b, const JsonValue* value);
