#include "json/json.h"

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

void Utf8Encode(StrBuf* b, u32 cp) {
  if (cp < 0x80) {
    StrBuf_AppendChar(b, (char)cp);
  } else if (cp < 0x800) {
    StrBuf_AppendChar(b, (char)(0xC0 | (cp >> 6)));
    StrBuf_AppendChar(b, (char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    StrBuf_AppendChar(b, (char)(0xE0 | (cp >> 12)));
    StrBuf_AppendChar(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    StrBuf_AppendChar(b, (char)(0x80 | (cp & 0x3F)));
  } else {
    StrBuf_AppendChar(b, (char)(0xF0 | (cp >> 18)));
    StrBuf_AppendChar(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
    StrBuf_AppendChar(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    StrBuf_AppendChar(b, (char)(0x80 | (cp & 0x3F)));
  }
}

int HexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
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
        int h0 = HexVal(s->p[0]), h1 = HexVal(s->p[1]), h2 = HexVal(s->p[2]), h3 = HexVal(s->p[3]);
        if ((h0 | h1 | h2 | h3) < 0) {
          s->ok = false;
          return StrView{"", 0};
        }
        u32 cp = ((u32)h0 << 12) | ((u32)h1 << 8) | ((u32)h2 << 4) | (u32)h3;
        s->p += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF && s->end - s->p >= 6 && s->p[0] == '\\' && s->p[1] == 'u') {
          int l0 = HexVal(s->p[2]), l1 = HexVal(s->p[3]), l2 = HexVal(s->p[4]), l3 = HexVal(s->p[5]);
          if ((l0 | l1 | l2 | l3) >= 0) {
            u32 lo = ((u32)l0 << 12) | ((u32)l1 << 8) | ((u32)l2 << 4) | (u32)l3;
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              s->p += 6;
            }
          }
        }
        Utf8Encode(&b, cp);
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
  return s.ok ? v : nullptr;
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

void Json_WriteString(StrBuf* b, StrView s) {
  StrBuf_AppendChar(b, '"');
  for (u32 i = 0; i < s.Length; i++) {
    char c = s.Data[i];
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
