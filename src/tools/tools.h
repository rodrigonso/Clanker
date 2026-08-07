#pragma once
#include "agent/tool.h"

// Built-in tool factories.
Tool ReadTool();

// Where the `shell` tool writes output that exceeds the truncation limit.
// `Directory` is re-read on every call so the owner can repoint it when the
// session changes; an empty view falls back to the OS temp directory, which is
// what `--no-session` runs get. The owner must only mutate it while no tool is
// running, since a tool batch reads it from worker threads.
struct ShellToolContext {
  StrView Directory;
};

Tool ShellTool(ShellToolContext* context = nullptr);
Tool WriteTool();
Tool EditTool();
Tool LsTool();
Tool FindTool();
Tool GrepTool();
