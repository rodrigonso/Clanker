#pragma once
#include "agent/ask.h"
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

// How the `ask_user` tool reaches the user. The owner installs a seam that
// knows about the renderer; the tool itself never does. A zero-initialized
// context means nobody is there to ask, and the tool reports that rather than
// blocking.
struct AskUserToolContext {
  AgentAskUser Ask;
};

Tool AskUserTool(AskUserToolContext* context = nullptr);
