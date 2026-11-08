#pragma once

#include "agent/hooks.h"

// Composes several independent AgentHarnessHooks into the one set a harness
// accepts, so unrelated features (an external policy gate, a memory store, a
// sandbox) can each own their callbacks and `User` without knowing about each
// other. Links run in registration order:
//
// - BeforeTool: the first link that blocks decides; later links are skipped.
// - AfterTool, RewriteToolArguments, BeforeRun, BeforeContext, ConvertToLlm,
//   BeforeProvider, AfterProvider, BeforeNavigation: a pipeline, each link
//   seeing the previous link's rewrite.
// - PrepareNextTurn: every link fills its own update; they merge field by
//   field, a later link winning wherever both set the same field.
// - ShouldStopAfterTurn: stops as soon as any link asks to.
// - GetSteeringMessages / GetFollowUpMessages: concatenated in link order.
// - BeforeCompaction: every link must succeed.
// - AfterTurn: every link runs; the first failure is reported afterwards.
//
// Every failing SessionResult stops the remaining links of that hook except
// AfterTurn, which is a notification.

constexpr u32 AGENT_HOOK_CHAIN_CAPACITY = 8;

struct AgentHookChain {
  AgentHarnessHooks Links[AGENT_HOOK_CHAIN_CAPACITY];
  u32 Count;
};

// False when the chain is full. The link is copied.
bool AgentHookChain_Add(AgentHookChain* chain, const AgentHarnessHooks* hooks);

// Hooks to install on AgentHarnessConfig::Hooks. An empty chain yields empty
// hooks and a single link is returned unchanged, so neither costs an extra
// call. Otherwise the result fans out through `chain`, which must then outlive
// every harness configured with it; only the slots some link fills are set.
AgentHarnessHooks AgentHookChain_Compose(AgentHookChain* chain);
