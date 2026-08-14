#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/array.h"

namespace {

struct P {
  const char* p;
  const char* end;
  Arena* a;
  bool ok;
};

void SkipWs(P* s) {
  while (s->p < s->end) {
    char c = *s->p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      s->p++;
    else
      break;
  }
}

JsonValue* NewVal(P* s, JsonKind k) {
  JsonValue* v = ArenaPushStruct(s->a, JsonValue);
  *v = JsonValue{};
  v->Kind = k;
  return v;
}

StrView ParseString(P* s) {
  s->p++;  // opening quote
  StrBuf b = StrBuf_Make(s->a, 16);
  while (s->p < s->end) {
    char c = *s->p++;
    if (c == '"') return StrBuf_View(&b);
    if (c != '\\') {
      StrBuf_AppendChar(&b, c);
      continue;
    }
    if (s->p >= s->end) break;
    char e = *s->p++;
    switch (e) {
      case '"': StrBuf_AppendChar(&b, '"'); break;
      case '\\': StrBuf_AppendChar(&b, '\\'); break;
      case '/': StrBuf_AppendChar(&b, '/'); break;
      case 'b': StrBuf_AppendChar(&b, '\b'); break;
      case 'f': StrBuf_AppendChar(&b, '\f'); break;
      case 'n': StrBuf_AppendChar(&b, '\n'); break;
      case 'r': StrBuf_AppendChar(&b, '\r'); break;
      case 't': StrBuf_AppendChar(&b, '\t'); break;
      case 'u': {
        if (s->end - s->p < 4) {
          s->ok = false;
          return StrView{"", 0};
        }
        int h0 = StrView_HexDigit(s->p[0]), h1 = StrView_HexDigit(s->p[1]), h2 = StrView_HexDigit(s->p[2]), h3 = StrView_HexDigit(s->p[3]);
        if ((h0 | h1 | h2 | h3) < 0) {
          s->ok = false;
          return StrView{"", 0};
        }
        u32 cp = ((u32)h0 << 12) | ((u32)h1 << 8) | ((u32)h2 << 4) | (u32)h3;
        s->p += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF && s->end - s->p >= 6 && s->p[0] == '\\' && s->p[1] == 'u') {
          int l0 = StrView_HexDigit(s->p[2]), l1 = StrView_HexDigit(s->p[3]), l2 = StrView_HexDigit(s->p[4]), l3 = StrView_HexDigit(s->p[5]);
          if ((l0 | l1 | l2 | l3) >= 0) {
            u32 lo = ((u32)l0 << 12) | ((u32)l1 << 8) | ((u32)l2 << 4) | (u32)l3;
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              s->p += 6;
            }
          }
        }
        StrBuf_AppendUtf8(&b, cp);
        break;
      }
      default:
        s->ok = false;
        return StrView{"", 0};
    }
  }
  s->ok = false;
  return StrView{"", 0};
}

JsonValue* ParseValue(P* s);

JsonValue* ParseObject(P* s) {
  s->p++;  // {
  Array<JsonMember> members = Array_Make<JsonMember>(s->a, 8);
  SkipWs(s);
  if (s->p < s->end && *s->p == '}') {
    s->p++;
    JsonValue* v = NewVal(s, JsonKind::Object);
    v->Members = members.Data;
    v->MemberCount = 0;
    return v;
  }
  while (s->p < s->end) {
    SkipWs(s);
    if (s->p >= s->end || *s->p != '"') {
      s->ok = false;
      return nullptr;
    }
    StrView key = ParseString(s);
    if (!s->ok) return nullptr;
    SkipWs(s);
    if (s->p >= s->end || *s->p != ':') {
      s->ok = false;
      return nullptr;
    }
    s->p++;
    JsonValue* val = ParseValue(s);
    if (!s->ok) return nullptr;
    JsonMember mm;
    mm.Key = key;
    mm.Value = val;
    Array_Push(&members, mm);
    SkipWs(s);
    if (s->p >= s->end) {
      s->ok = false;
      return nullptr;
    }
    if (*s->p == ',') {
      s->p++;
      continue;
    }
    if (*s->p == '}') {
      s->p++;
      break;
    }
    s->ok = false;
    return nullptr;
  }
  JsonValue* v = NewVal(s, JsonKind::Object);
  v->Members = members.Data;
  v->MemberCount = members.Count;
  return v;
}

JsonValue* ParseArray(P* s) {
  s->p++;  // [
  Array<JsonValue*> items = Array_Make<JsonValue*>(s->a, 8);
  SkipWs(s);
  if (s->p < s->end && *s->p == ']') {
    s->p++;
    JsonValue* v = NewVal(s, JsonKind::Array);
    v->Items = items.Data;
    v->Count = 0;
    return v;
  }
  while (s->p < s->end) {
    JsonValue* val = ParseValue(s);
    if (!s->ok) return nullptr;
    Array_Push(&items, val);
    SkipWs(s);
    if (s->p >= s->end) {
      s->ok = false;
      return nullptr;
    }
    if (*s->p == ',') {
      s->p++;
      continue;
    }
    if (*s->p == ']') {
      s->p++;
      break;
    }
    s->ok = false;
    return nullptr;
  }
  JsonValue* v = NewVal(s, JsonKind::Array);
  v->Items = items.Data;
  v->Count = items.Count;
  return v;
}

JsonValue* ParseValue(P* s) {
  SkipWs(s);
  if (s->p >= s->end) {
    s->ok = false;
    return nullptr;
  }
  char c = *s->p;
  if (c == '"') {
    JsonValue* v = NewVal(s, JsonKind::String);
    v->Str = ParseString(s);
    return s->ok ? v : nullptr;
  }
  if (c == '{') return ParseObject(s);
  if (c == '[') return ParseArray(s);
  if (c == 't') {
    if (s->end - s->p >= 4 && memcmp(s->p, "true", 4) == 0) {
      s->p += 4;
      JsonValue* v = NewVal(s, JsonKind::Bool);
      v->Bool = true;
      return v;
    }
    s->ok = false;
    return nullptr;
  }
  if (c == 'f') {
    if (s->end - s->p >= 5 && memcmp(s->p, "false", 5) == 0) {
      s->p += 5;
      JsonValue* v = NewVal(s, JsonKind::Bool);
      v->Bool = false;
      return v;
    }
    s->ok = false;
    return nullptr;
  }
  if (c == 'n') {
    if (s->end - s->p >= 4 && memcmp(s->p, "null", 4) == 0) {
      s->p += 4;
      return NewVal(s, JsonKind::Null);
    }
    s->ok = false;
    return nullptr;
  }
  // number
  char* endptr = nullptr;
  double d = strtod(s->p, &endptr);
  if (endptr == s->p) {
    s->ok = false;
    return nullptr;
  }
  s->p = endptr;
  JsonValue* v = NewVal(s, JsonKind::Number);
  v->Number = d;
  return v;
}

}  // namespace

JsonValue* Json_Parse(Arena* arena, StrView text) {
  P s;
  s.p = text.Data;
  s.end = text.Data + text.Length;
  s.a = arena;
  s.ok = true;
  JsonValue* v = ParseValue(&s);
  SkipWs(&s);
  return s.ok && s.p == s.end ? v : nullptr;
}

JsonValue* Json_Clone(Arena* arena, const JsonValue* value) {
  if (!value) return nullptr;
  JsonValue* clone = ArenaPushStruct(arena, JsonValue);
  *clone = *value;
  clone->Str = StrView_Dup(arena, value->Str);
  if (value->Kind == JsonKind::Array) {
    clone->Items = ArenaPushArray(arena, JsonValue*, value->Count ? value->Count : 1);
    for (u32 i = 0; i < value->Count; i++)
      clone->Items[i] = Json_Clone(arena, value->Items[i]);
  } else if (value->Kind == JsonKind::Object) {
    clone->Members =
        ArenaPushArray(arena, JsonMember, value->MemberCount ? value->MemberCount : 1);
    for (u32 i = 0; i < value->MemberCount; i++) {
      clone->Members[i].Key = StrView_Dup(arena, value->Members[i].Key);
      clone->Members[i].Value = Json_Clone(arena, value->Members[i].Value);
    }
  }
  return clone;
}

bool Json_Equals(const JsonValue* a, const JsonValue* b) {
  if (a == b) return true;
  if (!a || !b || a->Kind != b->Kind) return false;
  switch (a->Kind) {
    case JsonKind::Null: return true;
    case JsonKind::Bool: return a->Bool == b->Bool;
    case JsonKind::Number: return a->Number == b->Number;
    case JsonKind::String: return StrView_Eq(a->Str, b->Str);
    case JsonKind::Array:
      if (a->Count != b->Count) return false;
      for (u32 i = 0; i < a->Count; i++)
        if (!Json_Equals(a->Items[i], b->Items[i])) return false;
      return true;
    case JsonKind::Object:
      if (a->MemberCount != b->MemberCount) return false;
      for (u32 i = 0; i < a->MemberCount; i++) {
        const JsonValue* other = nullptr;
        for (u32 j = 0; j < b->MemberCount; j++) {
          if (StrView_Eq(a->Members[i].Key, b->Members[j].Key)) {
            other = b->Members[j].Value;
            break;
          }
        }
        if (!other || !Json_Equals(a->Members[i].Value, other)) return false;
      }
      return true;
  }
  return false;
}

JsonValue* Json_Get(const JsonValue* obj, const char* key) {
  if (!obj || obj->Kind != JsonKind::Object) return nullptr;
  StrView k = SV(key);
  for (u32 i = 0; i < obj->MemberCount; i++) {
    if (StrView_Eq(obj->Members[i].Key, k)) return obj->Members[i].Value;
  }
  return nullptr;
}

StrView Json_GetStr(const JsonValue* obj, const char* key) {
  JsonValue* v = Json_Get(obj, key);
  return (v && v->Kind == JsonKind::String) ? v->Str : StrView{"", 0};
}

f64 Json_GetNum(const JsonValue* obj, const char* key, f64 fallback) {
  JsonValue* v = Json_Get(obj, key);
  return (v && v->Kind == JsonKind::Number) ? v->Number : fallback;
}

i64 Json_GetInt(const JsonValue* obj, const char* key, i64 fallback) {
  JsonValue* v = Json_Get(obj, key);
  return (v && v->Kind == JsonKind::Number) ? (i64)v->Number : fallback;
}

bool Json_GetBool(const JsonValue* obj, const char* key, bool fallback) {
  JsonValue* v = Json_Get(obj, key);
  return (v && v->Kind == JsonKind::Bool) ? v->Bool : fallback;
}

// Length of the well-formed UTF-8 sequence at `s.Data[i]`, or 0 when the bytes
// there are not well-formed. Rejects overlong forms, the surrogate range, and
// anything above U+10FFFF -- exactly the cases providers reject.
static u32 Utf8SequenceLength(StrView s, u32 i) {
  u8 lead = (u8)s.Data[i];
  u8 minNext = 0x80;
  u8 maxNext = 0xBF;
  u32 length;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    if (lead == 0xE0)
      minNext = 0xA0;  // reject overlong 3-byte forms
    else if (lead == 0xED)
      maxNext = 0x9F;  // reject U+D800..U+DFFF encoded as CESU-8
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    if (lead == 0xF0)
      minNext = 0x90;  // reject overlong 4-byte forms
    else if (lead == 0xF4)
      maxNext = 0x8F;  // reject above U+10FFFF
  } else {
    return 0;  // stray continuation byte, C0/C1 overlong, or F5..FF
  }
  if (s.Length - i < length) return 0;
  u8 next = (u8)s.Data[i + 1];
  if (next < minNext || next > maxNext) return 0;
  for (u32 k = 2; k < length; k++) {
    u8 cont = (u8)s.Data[i + k];
    if (cont < 0x80 || cont > 0xBF) return 0;
  }
  return length;
}

void Json_WriteString(StrBuf* b, StrView s) {
  StrBuf_AppendChar(b, '"');
  for (u32 i = 0; i < s.Length; i++) {
    char c = s.Data[i];
    if ((u8)c >= 0x80) {
      // Every provider rejects a request body that is not well-formed UTF-8.
      // One bad byte -- a legacy-codepage shell byte, a chopped multi-byte
      // character, binary file content -- would otherwise fail this request and
      // every later one, since the poisoned text is replayed from history. This
      // is the single serialization choke point, so it is the one place that
      // has to hold the invariant.
      u32 length = Utf8SequenceLength(s, i);
      if (length) {
        StrBuf_Append(b, StrView{s.Data + i, length});
        i += length - 1;
      } else {
        StrBuf_AppendCStr(b, "\xEF\xBF\xBD");
      }
      continue;
    }
    switch (c) {
      case '"': StrBuf_AppendCStr(b, "\\\""); break;
      case '\\': StrBuf_AppendCStr(b, "\\\\"); break;
      case '\n': StrBuf_AppendCStr(b, "\\n"); break;
      case '\r': StrBuf_AppendCStr(b, "\\r"); break;
      case '\t': StrBuf_AppendCStr(b, "\\t"); break;
      case '\b': StrBuf_AppendCStr(b, "\\b"); break;
      case '\f': StrBuf_AppendCStr(b, "\\f"); break;
      default:
        if ((u8)c < 0x20) {
          static const char* hex = "0123456789abcdef";
          StrBuf_AppendCStr(b, "\\u00");
          StrBuf_AppendChar(b, hex[((u8)c >> 4) & 0xF]);
          StrBuf_AppendChar(b, hex[(u8)c & 0xF]);
        } else {
          StrBuf_AppendChar(b, c);
        }
    }
  }
  StrBuf_AppendChar(b, '"');
}

u32 Json_FormatNumber(char* dst, u32 cap, f64 value) {
  if (!dst || cap == 0) return 0;
  char candidate[64] = {};
  u32 length = 0;
  for (i32 precision = 1; precision <= 17; precision++) {
    int n = snprintf(candidate, sizeof(candidate), "%.*g", precision, value);
    if (n <= 0 || (u32)n >= sizeof(candidate)) continue;
    char* end = nullptr;
    f64 parsed = strtod(candidate, &end);
    u64 parsedBits = 0;
    u64 valueBits = 0;
    memcpy(&parsedBits, &parsed, sizeof(parsedBits));
    memcpy(&valueBits, &value, sizeof(valueBits));
    if (end && *end == '\0' && parsedBits == valueBits) {
      length = (u32)n;
      break;
    }
  }
  if (length == 0) {
    int n = snprintf(candidate, sizeof(candidate), "%.17g", value);
    if (n <= 0) return 0;
    length = (u32)n;
  }

  char normalized[64] = {};
  u32 normalizedLength = 0;
  u32 exponent = length;
  for (u32 i = 0; i < length; i++) {
    if (candidate[i] == 'e' || candidate[i] == 'E') {
      exponent = i;
      break;
    }
  }
  for (u32 i = 0; i < exponent; i++)
    normalized[normalizedLength++] = candidate[i];
  if (exponent < length) {
    normalized[normalizedLength++] = 'e';
    u32 i = exponent + 1;
    if (i < length && (candidate[i] == '+' || candidate[i] == '-'))
      normalized[normalizedLength++] = candidate[i++];
    while (i + 1 < length && candidate[i] == '0') i++;
    while (i < length) normalized[normalizedLength++] = candidate[i++];
  }

  u32 copied = normalizedLength < cap - 1 ? normalizedLength : cap - 1;
  if (copied) memcpy(dst, normalized, copied);
  dst[copied] = '\0';
  return copied;
}

void Json_WriteValue(StrBuf* b, const JsonValue* value) {
  if (!value) {
    StrBuf_AppendCStr(b, "null");
    return;
  }
  switch (value->Kind) {
    case JsonKind::Null:
      StrBuf_AppendCStr(b, "null");
      break;
    case JsonKind::Bool:
      StrBuf_AppendCStr(b, value->Bool ? "true" : "false");
      break;
    case JsonKind::Number: {
      char number[64];
      u32 n = Json_FormatNumber(number, (u32)sizeof(number), value->Number);
      if (n) StrBuf_Append(b, StrView{number, n});
    } break;
    case JsonKind::String:
      Json_WriteString(b, value->Str);
      break;
    case JsonKind::Array:
      StrBuf_AppendChar(b, '[');
      for (u32 i = 0; i < value->Count; i++) {
        if (i) StrBuf_AppendChar(b, ',');
        Json_WriteValue(b, value->Items[i]);
      }
      StrBuf_AppendChar(b, ']');
      break;
    case JsonKind::Object:
      StrBuf_AppendChar(b, '{');
      for (u32 i = 0; i < value->MemberCount; i++) {
        if (i) StrBuf_AppendChar(b, ',');
        Json_WriteString(b, value->Members[i].Key);
        StrBuf_AppendChar(b, ':');
        Json_WriteValue(b, value->Members[i].Value);
      }
      StrBuf_AppendChar(b, '}');
      break;
  }
}
