#include <string.h>

#include "base/arena.h"
#include "check.h"
#include "json/json.h"
#include "json/schema.h"

void TestJson() {
  Arena a = ArenaCreate();

  // Object with nested array, escapes, numbers, bools, null.
  const char* doc =
      "{\"name\":\"pi\",\"n\":42,\"f\":3.5,\"ok\":true,\"nope\":false,\"nil\":null,"
      "\"esc\":\"line1\\nline2\\t\\\"q\\\"\",\"arr\":[1,2,3],\"obj\":{\"k\":\"v\"}}";
  JsonValue* root = Json_Parse(&a, SV(doc));
  CHECK(Json_IsObject(root));
  CHECK_ALL(StrView_EqCStr(Json_GetStr(root, "name"), "pi"),
            Json_GetInt(root, "n", 0) == 42, Json_GetNum(root, "f", 0) == 3.5,
            Json_GetBool(root, "ok", false) == true,
            Json_GetBool(root, "nope", true) == false);
  JsonValue* nil = Json_Get(root, "nil");
  CHECK_ALL(nil && nil->Kind == JsonKind::Null,
            StrView_EqCStr(Json_GetStr(root, "esc"), "line1\nline2\t\"q\""));

  JsonValue* arr = Json_Get(root, "arr");
  CHECK_ALL(Json_IsArray(arr), arr->Count == 3,
            arr->Items[1]->Kind == JsonKind::Number &&
                (i64)arr->Items[1]->Number == 2);

  JsonValue* obj = Json_Get(root, "obj");
  CHECK(StrView_EqCStr(Json_GetStr(obj, "k"), "v"));

  // Unicode escape decodes to UTF-8.
  JsonValue* uni = Json_Parse(&a, SV("{\"s\":\"\\u00e9\\u4e2d\"}"));
  StrView s = Json_GetStr(uni, "s");
  CHECK_ALL(s.Length == 5,  // é (2 bytes) + 中 (3 bytes)
            (u8)s.Data[0] == 0xC3 && (u8)s.Data[1] == 0xA9,
            (u8)s.Data[2] == 0xE4 && (u8)s.Data[3] == 0xB8 &&
                (u8)s.Data[4] == 0xAD);

  // Surrogate pair (emoji U+1F600).
  JsonValue* emo = Json_Parse(&a, SV("{\"e\":\"\\ud83d\\ude00\"}"));
  StrView es = Json_GetStr(emo, "e");
  CHECK(es.Length == 4 && (u8)es.Data[0] == 0xF0);

  // Writer escaping round-trips through the parser.
  StrBuf w = StrBuf_Make(&a, 16);
  Json_WriteString(&w, SV("a\"b\\c\nd"));
  StrBuf wrapped = StrBuf_Make(&a, 16);
  StrBuf_AppendCStr(&wrapped, "{\"x\":");
  StrBuf_Append(&wrapped, StrBuf_View(&w));
  StrBuf_AppendCStr(&wrapped, "}");
  JsonValue* rt = Json_Parse(&a, StrBuf_View(&wrapped));
  CHECK(StrView_EqCStr(Json_GetStr(rt, "x"), "a\"b\\c\nd"));

  // Invalid input returns nullptr.
  CHECK(Json_Parse(&a, SV("{bad")) == nullptr);
  CHECK(Json_Parse(&a, SV("{} trailing")) == nullptr);

  // Tool schemas are compiled once, coerce primitive arguments like Pi, and
  // reject unsupported validation keywords instead of validating partially.
  JsonSchema schema = {};
  StrBuf schemaErr = StrBuf_Make(&a, 128);
  CHECK(JsonSchema_Compile(
      &a,
      SV("{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\","
         "\"minimum\":1},\"enabled\":{\"type\":\"boolean\"},\"items\":{\"type\":"
         "\"array\",\"items\":{\"type\":\"string\"},\"minItems\":1}},\"required\":"
         "[\"count\",\"enabled\",\"items\"],\"additionalProperties\":false}"),
      &schema, &schemaErr));
  JsonValue* coerced =
      Json_Parse(&a, SV("{\"count\":\"2\",\"enabled\":1,\"items\":[3]}"));
  StrBuf validationErr = StrBuf_Make(&a, 128);
  CHECK(JsonSchema_Validate(&a, &schema, coerced, &validationErr));
  StrBuf canonical = StrBuf_Make(&a, 128);
  Json_WriteValue(&canonical, coerced);
  CHECK(StrView_EqCStr(StrBuf_View(&canonical),
                      "{\"count\":2,\"enabled\":true,\"items\":[\"3\"]}"));

  JsonSchema firstValid = {};
  StrBuf firstValidSchemaErr = StrBuf_Make(&a, 128);
  CHECK(JsonSchema_Compile(
      &a,
      SV("{\"oneOf\":[{\"type\":\"string\"},{\"type\":\"number\"}]}"),
      &firstValid, &firstValidSchemaErr));
  JsonValue* ambiguous = Json_Parse(&a, SV("\"1\""));
  StrBuf ambiguousErr = StrBuf_Make(&a, 128);
  CHECK(JsonSchema_Validate(&a, &firstValid, ambiguous, &ambiguousErr));
  CHECK(ambiguous->Kind == JsonKind::String);

  JsonValue* readableNumber = Json_Parse(&a, SV("{\"n\":0.1}"));
  StrBuf readable = StrBuf_Make(&a, 32);
  Json_WriteValue(&readable, readableNumber);
  CHECK(StrView_EqCStr(StrBuf_View(&readable), "{\"n\":0.1}"));

  JsonValue* invalid =
      Json_Parse(&a, SV("{\"count\":0,\"enabled\":true,\"items\":[]}"));
  StrBuf invalidErr = StrBuf_Make(&a, 128);
  CHECK(!JsonSchema_Validate(&a, &schema, invalid, &invalidErr));
  CHECK(StrView_Contains(StrBuf_View(&invalidErr), "root.count"));

  JsonSchema unsupported = {};
  StrBuf unsupportedErr = StrBuf_Make(&a, 128);
  CHECK(!JsonSchema_Compile(
      &a, SV("{\"type\":\"string\",\"pattern\":\"^[a-z]+$\"}"),
      &unsupported, &unsupportedErr));
  CHECK(StrView_Contains(StrBuf_View(&unsupportedErr),
                         "unsupported schema keyword"));

  ArenaRelease(&a);
}
