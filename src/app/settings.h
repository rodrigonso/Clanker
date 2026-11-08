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
  AgentCompactionPolicy Compaction = AgentCompactionPolicy_Default();
  AgentRetryPolicy Retry = AgentRetryPolicy_Default();
  AgentTurnLimitPolicy TurnLimit = AgentTurnLimitPolicy_Default();
  AgentPermissionMode PermissionMode = AgentPermissionMode::Auto;
  StrView PermissionReviewerModel = AGENT_PERMISSION_DEFAULT_REVIEWER_MODEL;
  f64 PermissionReviewerThreshold = AGENT_PERMISSION_DEFAULT_REVIEWER_THRESHOLD;
  // Load-only diagnostics; never written to settings.json.
  bool PermissionReviewerInvalid = false;
  bool PermissionReviewerThresholdInvalid = false;
  bool YoloAcknowledged = false;
  bool Rounded = true;
  // The memory wiki; `--no-memory` and CLANKER_NO_MEMORY turn it off for one
  // run without changing this.
  bool Memory = true;
  // Model dreaming uses instead of the active one. Empty means the same model.
  StrView DreamModel;
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
