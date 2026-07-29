#pragma once

#include "base/basics.h"

// Zero is intentionally the conservative default for zero-initialized custom
// tools. Recovery never repeats a possibly mutating effect without a decision.
enum class ToolReplayPolicy : u8 {
  ManualReconcile,
  ReplaySafe,
  NoEffect,
};
