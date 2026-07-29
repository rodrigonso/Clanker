#include "json/schema.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

bool Eq(StrView value, const char* text) { return StrView_EqCStr(value, text); }

void AppendPath(StrBuf* out, StrView base, StrView segment) {
  StrBuf_Append(out, base.Length ? base : SVL("root"));
  if (segment.Length) {
    if (segment.Data[0] != '[') StrBuf_AppendChar(out, '.');
    StrBuf_Append(out, segment);
  }
}

bool Fail(StrBuf* err, StrView path, const char* message) {
  AppendPath(err, path, {});
  StrBuf_AppendCStr(err, ": ");
  StrBuf_AppendCStr(err, message);
  return false;
}

bool IsNonNegativeInteger(const JsonValue* value) {
  return value && value->Kind == JsonKind::Number && value->Number >= 0 &&
         isfinite(value->Number) && floor(value->Number) == value->Number;
}

bool IsKnownType(StrView type) {
  return Eq(type, "null") || Eq(type, "boolean") || Eq(type, "number") ||
         Eq(type, "integer") || Eq(type, "string") || Eq(type, "array") ||
         Eq(type, "object");
}

bool ValidateSchemaNode(const JsonValue* schema, StrBuf* err, StrView path);

bool ValidateSchemaArray(const JsonValue* array, StrBuf* err, StrView path,
                         bool requireNonEmpty) {
  if (!Json_IsArray(array) || (requireNonEmpty && array->Count == 0))
    return Fail(err, path, "must be a non-empty schema array");
  for (u32 i = 0; i < array->Count; i++) {
    char index[32];
    int n = snprintf(index, sizeof(index), "[%u]", i);
    StrBuf child = StrBuf_Make(err->Backing, path.Length + (u32)n + 1);
    StrBuf_Append(&child, path);
    StrBuf_Append(&child, StrView{index, (u32)n});
    if (!ValidateSchemaNode(array->Items[i], err, StrBuf_View(&child)))
      return false;
  }
  return true;
}

bool ValidateSchemaNode(const JsonValue* schema, StrBuf* err, StrView path) {
  if (!Json_IsObject(schema)) return Fail(err, path, "schema must be an object");

  for (u32 i = 0; i < schema->MemberCount; i++) {
    StrView key = schema->Members[i].Key;
    const JsonValue* value = schema->Members[i].Value;
    bool annotation = Eq(key, "$schema") || Eq(key, "$id") ||
                      Eq(key, "title") || Eq(key, "description") ||
                      Eq(key, "default") || Eq(key, "examples") ||
                      Eq(key, "readOnly") || Eq(key, "writeOnly") ||
                      Eq(key, "deprecated");
    if (annotation) continue;

    if (Eq(key, "type")) {
      if (Json_IsString(value)) {
        if (!IsKnownType(value->Str))
          return Fail(err, path, "contains an unsupported type");
      } else if (Json_IsArray(value) && value->Count > 0) {
        for (u32 t = 0; t < value->Count; t++)
          if (!Json_IsString(value->Items[t]) ||
              !IsKnownType(value->Items[t]->Str))
            return Fail(err, path, "type union contains an unsupported value");
      } else {
        return Fail(err, path, "type must be a string or non-empty array");
      }
      continue;
    }

    if (Eq(key, "properties")) {
      if (!Json_IsObject(value))
        return Fail(err, path, "properties must be an object");
      for (u32 p = 0; p < value->MemberCount; p++) {
        StrBuf child = StrBuf_Make(err->Backing,
                                  path.Length + value->Members[p].Key.Length + 2);
        StrBuf_Append(&child, path);
        StrBuf_AppendChar(&child, '.');
        StrBuf_Append(&child, value->Members[p].Key);
        if (!ValidateSchemaNode(value->Members[p].Value, err,
                                StrBuf_View(&child)))
          return false;
      }
      continue;
    }

    if (Eq(key, "required")) {
      if (!Json_IsArray(value))
        return Fail(err, path, "required must be an array");
      for (u32 r = 0; r < value->Count; r++)
        if (!Json_IsString(value->Items[r]))
          return Fail(err, path, "required entries must be strings");
      continue;
    }

    if (Eq(key, "additionalProperties")) {
      if (value->Kind != JsonKind::Bool && !Json_IsObject(value))
        return Fail(err, path,
                    "additionalProperties must be boolean or a schema");
      if (Json_IsObject(value) &&
          !ValidateSchemaNode(value, err, path))
        return false;
      continue;
    }

    if (Eq(key, "items")) {
      if (Json_IsObject(value)) {
        if (!ValidateSchemaNode(value, err, path)) return false;
      } else if (Json_IsArray(value)) {
        if (!ValidateSchemaArray(value, err, path, false)) return false;
      } else {
        return Fail(err, path, "items must be a schema or schema array");
      }
      continue;
    }

    if (Eq(key, "allOf") || Eq(key, "anyOf") || Eq(key, "oneOf")) {
      if (!ValidateSchemaArray(value, err, path, true)) return false;
      continue;
    }

    if (Eq(key, "enum")) {
      if (!Json_IsArray(value) || value->Count == 0)
        return Fail(err, path, "enum must be a non-empty array");
      continue;
    }
    if (Eq(key, "const")) continue;

    if (Eq(key, "minimum") || Eq(key, "maximum") ||
        Eq(key, "exclusiveMinimum") || Eq(key, "exclusiveMaximum") ||
        Eq(key, "multipleOf")) {
      if (!value || value->Kind != JsonKind::Number ||
          (Eq(key, "multipleOf") && value->Number <= 0))
        return Fail(err, path, "numeric constraint is invalid");
      continue;
    }

    if (Eq(key, "minLength") || Eq(key, "maxLength") ||
        Eq(key, "minItems") || Eq(key, "maxItems") ||
        Eq(key, "minProperties") || Eq(key, "maxProperties")) {
      if (!IsNonNegativeInteger(value))
        return Fail(err, path, "size constraint must be a non-negative integer");
      continue;
    }

    if (Eq(key, "uniqueItems")) {
      if (!value || value->Kind != JsonKind::Bool)
        return Fail(err, path, "uniqueItems must be boolean");
      continue;
    }

    return Fail(err, path, "contains an unsupported schema keyword");
  }
  return true;
}

bool MatchesType(const JsonValue* value, StrView type) {
  if (Eq(type, "null")) return value->Kind == JsonKind::Null;
  if (Eq(type, "boolean")) return value->Kind == JsonKind::Bool;
  if (Eq(type, "number")) return value->Kind == JsonKind::Number;
  if (Eq(type, "integer"))
    return value->Kind == JsonKind::Number && isfinite(value->Number) &&
           floor(value->Number) == value->Number;
  if (Eq(type, "string")) return value->Kind == JsonKind::String;
  if (Eq(type, "array")) return value->Kind == JsonKind::Array;
  if (Eq(type, "object")) return value->Kind == JsonKind::Object;
  return false;
}

bool ConvertStringNumber(Arena* arena, JsonValue* value, bool integer) {
  if (value->Kind != JsonKind::String || value->Str.Length == 0) return false;
  StrBuf text = StrBuf_Make(arena, value->Str.Length + 1);
  StrBuf_Append(&text, value->Str);
  char* end = nullptr;
  f64 parsed = strtod(StrBuf_CStr(&text), &end);
  if (!end || *end || !isfinite(parsed) ||
      (integer && floor(parsed) != parsed))
    return false;
  *value = JsonValue{};
  value->Kind = JsonKind::Number;
  value->Number = parsed;
  return true;
}

bool CoerceToType(Arena* arena, JsonValue* value, StrView type) {
  if (MatchesType(value, type)) return true;
  if (Eq(type, "number") || Eq(type, "integer")) {
    bool integer = Eq(type, "integer");
    if (value->Kind == JsonKind::Null) {
      *value = JsonValue{};
      value->Kind = JsonKind::Number;
      return true;
    }
    if (value->Kind == JsonKind::Bool) {
      bool boolean = value->Bool;
      *value = JsonValue{};
      value->Kind = JsonKind::Number;
      value->Number = boolean ? 1 : 0;
      return true;
    }
    return ConvertStringNumber(arena, value, integer);
  }
  if (Eq(type, "boolean")) {
    if (value->Kind == JsonKind::Null) {
      *value = JsonValue{};
      value->Kind = JsonKind::Bool;
      return true;
    }
    if (value->Kind == JsonKind::String) {
      if (!Eq(value->Str, "true") && !Eq(value->Str, "false")) return false;
      bool boolean = Eq(value->Str, "true");
      *value = JsonValue{};
      value->Kind = JsonKind::Bool;
      value->Bool = boolean;
      return true;
    }
    if (value->Kind == JsonKind::Number &&
        (value->Number == 0 || value->Number == 1)) {
      bool boolean = value->Number == 1;
      *value = JsonValue{};
      value->Kind = JsonKind::Bool;
      value->Bool = boolean;
      return true;
    }
    return false;
  }
  if (Eq(type, "string")) {
    if (value->Kind == JsonKind::Null) {
      *value = JsonValue{};
      value->Kind = JsonKind::String;
      value->Str = SVL("");
      return true;
    }
    if (value->Kind == JsonKind::Bool) {
      bool boolean = value->Bool;
      *value = JsonValue{};
      value->Kind = JsonKind::String;
      value->Str = boolean ? SVL("true") : SVL("false");
      return true;
    }
    if (value->Kind == JsonKind::Number) {
      char number[64];
      u32 n =
          Json_FormatNumber(number, (u32)sizeof(number), value->Number);
      *value = JsonValue{};
      value->Kind = JsonKind::String;
      value->Str = StrView_Dup(arena, StrView{number, n});
      return true;
    }
    return false;
  }
  if (Eq(type, "null") &&
      ((value->Kind == JsonKind::String && value->Str.Length == 0) ||
       (value->Kind == JsonKind::Number && value->Number == 0) ||
       (value->Kind == JsonKind::Bool && !value->Bool))) {
    *value = JsonValue{};
    value->Kind = JsonKind::Null;
    return true;
  }
  return false;
}

u32 Utf8Length(StrView value) {
  u32 count = 0;
  for (u32 i = 0; i < value.Length; i++) {
    if (((u8)value.Data[i] & 0xC0) != 0x80) count++;
  }
  return count;
}

bool ValidateNode(Arena* arena, const JsonValue* schema, JsonValue* value,
                  StrBuf* err, StrView path);

bool ValidateComposition(Arena* arena, const JsonValue* schemas,
                         JsonValue* value, StrBuf* err, StrView path) {
  for (u32 i = 0; i < schemas->Count; i++) {
    JsonValue* candidate = Json_Clone(arena, value);
    StrBuf ignored = StrBuf_Make(arena, 64);
    if (ValidateNode(arena, schemas->Items[i], candidate, &ignored, path)) {
      *value = *candidate;
      return true;
    }
  }
  return Fail(err, path, "must match at least one schema");
}

bool CheckSize(const JsonValue* schema, const char* minKey,
               const char* maxKey, u32 size, StrBuf* err, StrView path) {
  const JsonValue* minimum = Json_Get(schema, minKey);
  const JsonValue* maximum = Json_Get(schema, maxKey);
  if (minimum && size < (u32)minimum->Number)
    return Fail(err, path, "is shorter or smaller than allowed");
  if (maximum && size > (u32)maximum->Number)
    return Fail(err, path, "is longer or larger than allowed");
  return true;
}

bool ValidateNode(Arena* arena, const JsonValue* schema, JsonValue* value,
                  StrBuf* err, StrView path) {
  const JsonValue* allOf = Json_Get(schema, "allOf");
  if (allOf) {
    for (u32 i = 0; i < allOf->Count; i++)
      if (!ValidateNode(arena, allOf->Items[i], value, err, path))
        return false;
  }
  const JsonValue* anyOf = Json_Get(schema, "anyOf");
  if (anyOf && !ValidateComposition(arena, anyOf, value, err, path))
    return false;
  const JsonValue* oneOf = Json_Get(schema, "oneOf");
  if (oneOf && !ValidateComposition(arena, oneOf, value, err, path))
    return false;

  const JsonValue* type = Json_Get(schema, "type");
  if (type) {
    bool matches = false;
    if (Json_IsString(type)) {
      if (!MatchesType(value, type->Str))
        CoerceToType(arena, value, type->Str);
      matches = MatchesType(value, type->Str);
    } else {
      for (u32 i = 0; i < type->Count; i++)
        if (MatchesType(value, type->Items[i]->Str)) matches = true;
      if (!matches) {
        for (u32 i = 0; i < type->Count; i++) {
          JsonValue* candidate = Json_Clone(arena, value);
          if (CoerceToType(arena, candidate, type->Items[i]->Str)) {
            *value = *candidate;
            matches = true;
            break;
          }
        }
      }
    }
    if (!matches) return Fail(err, path, "has the wrong type");
  }

  const JsonValue* constant = Json_Get(schema, "const");
  if (constant && !Json_Equals(value, constant))
    return Fail(err, path, "does not match the required constant");
  const JsonValue* enumeration = Json_Get(schema, "enum");
  if (enumeration) {
    bool found = false;
    for (u32 i = 0; i < enumeration->Count; i++)
      if (Json_Equals(value, enumeration->Items[i])) found = true;
    if (!found) return Fail(err, path, "is not an allowed enum value");
  }

  if (value->Kind == JsonKind::Object) {
    if (!CheckSize(schema, "minProperties", "maxProperties",
                   value->MemberCount, err, path))
      return false;
    const JsonValue* required = Json_Get(schema, "required");
    if (required) {
      for (u32 i = 0; i < required->Count; i++) {
        StrBuf key = StrBuf_Make(arena, required->Items[i]->Str.Length + 1);
        StrBuf_Append(&key, required->Items[i]->Str);
        if (!Json_Get(value, StrBuf_CStr(&key))) {
          StrBuf missing = StrBuf_Make(err->Backing,
                                      path.Length + required->Items[i]->Str.Length + 2);
          StrBuf_Append(&missing, path);
          StrBuf_AppendChar(&missing, '.');
          StrBuf_Append(&missing, required->Items[i]->Str);
          return Fail(err, StrBuf_View(&missing), "is required");
        }
      }
    }

    const JsonValue* properties = Json_Get(schema, "properties");
    const JsonValue* additional = Json_Get(schema, "additionalProperties");
    for (u32 i = 0; i < value->MemberCount; i++) {
      const JsonValue* propertySchema = nullptr;
      if (properties) {
        for (u32 p = 0; p < properties->MemberCount; p++)
          if (StrView_Eq(properties->Members[p].Key, value->Members[i].Key))
            propertySchema = properties->Members[p].Value;
      }
      StrBuf child = StrBuf_Make(err->Backing,
                                path.Length + value->Members[i].Key.Length + 2);
      StrBuf_Append(&child, path);
      StrBuf_AppendChar(&child, '.');
      StrBuf_Append(&child, value->Members[i].Key);
      if (propertySchema) {
        if (!ValidateNode(arena, propertySchema, value->Members[i].Value, err,
                          StrBuf_View(&child)))
          return false;
      } else if (additional && additional->Kind == JsonKind::Bool &&
                 !additional->Bool) {
        return Fail(err, StrBuf_View(&child), "is not an allowed property");
      } else if (Json_IsObject(additional) &&
                 !ValidateNode(arena, additional, value->Members[i].Value, err,
                               StrBuf_View(&child))) {
        return false;
      }
    }
  } else if (value->Kind == JsonKind::Array) {
    if (!CheckSize(schema, "minItems", "maxItems", value->Count, err, path))
      return false;
    const JsonValue* unique = Json_Get(schema, "uniqueItems");
    if (unique && unique->Bool) {
      for (u32 i = 0; i < value->Count; i++)
        for (u32 j = i + 1; j < value->Count; j++)
          if (Json_Equals(value->Items[i], value->Items[j]))
            return Fail(err, path, "contains duplicate items");
    }
    const JsonValue* items = Json_Get(schema, "items");
    if (Json_IsObject(items)) {
      for (u32 i = 0; i < value->Count; i++) {
        char index[32];
        int n = snprintf(index, sizeof(index), "[%u]", i);
        StrBuf child = StrBuf_Make(err->Backing, path.Length + (u32)n + 1);
        StrBuf_Append(&child, path);
        StrBuf_Append(&child, StrView{index, (u32)n});
        if (!ValidateNode(arena, items, value->Items[i], err,
                          StrBuf_View(&child)))
          return false;
      }
    } else if (Json_IsArray(items)) {
      u32 count = value->Count < items->Count ? value->Count : items->Count;
      for (u32 i = 0; i < count; i++) {
        char index[32];
        int n = snprintf(index, sizeof(index), "[%u]", i);
        StrBuf child = StrBuf_Make(err->Backing, path.Length + (u32)n + 1);
        StrBuf_Append(&child, path);
        StrBuf_Append(&child, StrView{index, (u32)n});
        if (!ValidateNode(arena, items->Items[i], value->Items[i], err,
                          StrBuf_View(&child)))
          return false;
      }
    }
  } else if (value->Kind == JsonKind::String) {
    if (!CheckSize(schema, "minLength", "maxLength", Utf8Length(value->Str),
                   err, path))
      return false;
  } else if (value->Kind == JsonKind::Number) {
    const JsonValue* minimum = Json_Get(schema, "minimum");
    const JsonValue* maximum = Json_Get(schema, "maximum");
    const JsonValue* exclusiveMinimum = Json_Get(schema, "exclusiveMinimum");
    const JsonValue* exclusiveMaximum = Json_Get(schema, "exclusiveMaximum");
    const JsonValue* multiple = Json_Get(schema, "multipleOf");
    if (minimum && value->Number < minimum->Number)
      return Fail(err, path, "is below the minimum");
    if (maximum && value->Number > maximum->Number)
      return Fail(err, path, "is above the maximum");
    if (exclusiveMinimum && value->Number <= exclusiveMinimum->Number)
      return Fail(err, path, "is not above the exclusive minimum");
    if (exclusiveMaximum && value->Number >= exclusiveMaximum->Number)
      return Fail(err, path, "is not below the exclusive maximum");
    if (multiple) {
      f64 quotient = value->Number / multiple->Number;
      if (fabs(quotient - round(quotient)) > 1e-9)
        return Fail(err, path, "is not a valid multiple");
    }
  }

  return true;
}

}  // namespace

bool JsonSchema_Compile(Arena* arena, StrView schemaJson, JsonSchema* out,
                        StrBuf* err) {
  if (!arena || !out || !err) return false;
  JsonValue* root = Json_Parse(arena, schemaJson);
  if (!root) return Fail(err, SVL("root"), "schema is invalid JSON");
  if (!ValidateSchemaNode(root, err, SVL("root"))) return false;
  out->Root = root;
  return true;
}

bool JsonSchema_Validate(Arena* arena, const JsonSchema* schema,
                         JsonValue* value, StrBuf* err) {
  if (!arena || !schema || !schema->Root || !value || !err) return false;
  return ValidateNode(arena, schema->Root, value, err, SVL("root"));
}
