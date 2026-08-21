#pragma once
#include "agent/compaction.h"
#include "agent/primitives.h"
#include "base/arena.h"
#include "base/array.h"
#include "base/basics.h"
#include "base/str.h"
#include "agent/permissions.h"
#include "app/project_trust.h"
#include "llm/types.h"

struct AppSettings {
  StrView Provider;
  StrView Model;
  DefaultProjectTrust DefaultTrust;
  ModelRequestProfile Profile;
  AgentCompactionPolicy Compaction;
  AgentRetryPolicy Retry = AgentRetryPolicy_Default();
  AgentTurnLimitPolicy TurnLimit = AgentTurnLimitPolicy_Default();
  AgentPermissionMode PermissionMode = AgentPermissionMode::Auto;
  StrView PermissionReviewerModel = AGENT_PERMISSION_DEFAULT_REVIEWER_MODEL;
  bool YoloAcknowledged = false;
  bool Rounded = true;
  Array<StrView> Skills;
  Array<StrView> PromptTemplates;
};

enum class AppSettingsLoadResult : u8 {
  Success,
  Missing,
  Malformed,
  ReadFailure,
};

// Default path: <home>/.clanker/settings.json.
StrView AppSettings_DefaultPath(Arena* arena);

// Successful strings are owned by `arena`.
AppSettingsLoadResult AppSettings_Load(Arena* arena, StrView path, AppSettings* out);
bool AppSettings_Save(Arena* arena, StrView path, const AppSettings* settings);
