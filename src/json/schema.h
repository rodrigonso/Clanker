#pragma once

#include "json/json.h"

struct JsonSchema {
  const JsonValue* Root;
};

// Parses a schema once and rejects unsupported behavioral keywords so callers
// never mistake partial validation for full validation.
bool JsonSchema_Compile(Arena* arena, StrView schemaJson, JsonSchema* out,
                        StrBuf* err);

// Coerces primitive values using Pi-compatible rules, then validates in place.
bool JsonSchema_Validate(Arena* arena, const JsonSchema* schema,
                         JsonValue* value, StrBuf* err);

