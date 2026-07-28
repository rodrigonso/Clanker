#pragma once
#include "base/arena.h"
#include "base/basics.h"
#include "base/str.h"
#include "app/project_trust.h"

struct AppSettings {
  StrView Provider;
  StrView Model;
  DefaultProjectTrust DefaultTrust;
};

enum class AppSettingsLoadResult : u8 {
  Success,
  Missing,
  Malformed,
  ReadFailure,
};

// Default path: <home>/.clank/settings.json.
StrView AppSettings_DefaultPath(Arena* arena);

// Successful strings are owned by `arena`.
AppSettingsLoadResult AppSettings_Load(Arena* arena, StrView path, AppSettings* out);
bool AppSettings_Save(Arena* arena, StrView path, const AppSettings* settings);
