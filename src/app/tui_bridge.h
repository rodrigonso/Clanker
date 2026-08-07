#pragma once

#include "agent/harness.h"
#include "tui/tui.h"

// Adapter between the agent and the TUI, which know nothing about each other.
//
// The TUI takes progress as ordinary calls and asks for steer/cancel/is-busy
// through TuiAgentHooks, so this is the one place that understands both. It is
// a call adapter, not a translation layer: no intermediate event struct exists
// and nothing is copied that the TUI calls do not already carry.

// Translates one watch event into the matching Tui_* reporting call. Suitable
// as a HarnessEventSink callback with `user` set to a Tui*.
void TuiBridge_HarnessEvent(void* user, const AgentHarnessEvent* ev);

// Hooks backed by `harness`, for Tui::Agent.
TuiAgentHooks TuiBridge_AgentHooks(AgentHarness* harness);
