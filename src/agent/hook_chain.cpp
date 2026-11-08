#include "agent/hook_chain.h"

namespace {

template <typename Fn>
bool AnyLink(const AgentHookChain* chain, Fn AgentHarnessHooks::*slot) {
  for (u32 i = 0; i < chain->Count; i++)
    if (chain->Links[i].*slot) return true;
  return false;
}

// Runs one SessionResult hook across the chain, stopping at the first failure.
// Arguments are forwarded unchanged, so a pointer argument carries each link's
// rewrite into the next.
template <typename... Args>
SessionResult Pipeline(void* user,
                       SessionResult (*AgentHarnessHooks::*slot)(void*, Args...),
                       Args... args) {
  const AgentHookChain* chain = (const AgentHookChain*)user;
  for (u32 i = 0; i < chain->Count; i++) {
    const AgentHarnessHooks& link = chain->Links[i];
    if (!(link.*slot)) continue;
    SessionResult result = (link.*slot)(link.User, args...);
    if (!SessionResult_IsOk(result)) return result;
  }
  return SessionResult_Ok();
}

SessionResult BeforeRun(void* user, Arena* arena, Message* ioPrompt) {
  return Pipeline(user, &AgentHarnessHooks::BeforeRun, arena, ioPrompt);
}

SessionResult BeforeContext(void* user, Arena* arena, AgentTurnKind turn,
                            DurableId headId, Array<Message>* ioMessages) {
  return Pipeline(user, &AgentHarnessHooks::BeforeContext, arena, turn, headId,
                  ioMessages);
}

SessionResult ConvertToLlm(void* user, Arena* arena, AgentTurnKind turn,
                           DurableId headId, Array<Message>* ioMessages) {
  return Pipeline(user, &AgentHarnessHooks::ConvertToLlm, arena, turn, headId,
                  ioMessages);
}

SessionResult BeforeProvider(void* user, Arena* arena, AgentTurnKind turn,
                             u32 attemptIndex, AgentHarnessProviderInput* io) {
  return Pipeline(user, &AgentHarnessHooks::BeforeProvider, arena, turn,
                  attemptIndex, io);
}

SessionResult AfterProvider(void* user, Arena* arena, AgentTurnKind turn,
                            u32 attemptIndex, Message* ioAssistant) {
  return Pipeline(user, &AgentHarnessHooks::AfterProvider, arena, turn,
                  attemptIndex, ioAssistant);
}

SessionResult RewriteToolArguments(void* user, Arena* arena, StrView toolName,
                                   StrView* ioArgumentsJson) {
  return Pipeline(user, &AgentHarnessHooks::RewriteToolArguments, arena, toolName,
                  ioArgumentsJson);
}

SessionResult GetSteeringMessages(void* user, Arena* arena,
                                  Array<Message>* outMessages) {
  return Pipeline(user, &AgentHarnessHooks::GetSteeringMessages, arena, outMessages);
}

SessionResult GetFollowUpMessages(void* user, Arena* arena,
                                  Array<Message>* outMessages) {
  return Pipeline(user, &AgentHarnessHooks::GetFollowUpMessages, arena, outMessages);
}

SessionResult BeforeCompaction(void* user, Arena* arena, DurableId headId) {
  return Pipeline(user, &AgentHarnessHooks::BeforeCompaction, arena, headId);
}

SessionResult BeforeNavigation(void* user, Arena* arena,
                               DurableId* ioTargetEntryId) {
  return Pipeline(user, &AgentHarnessHooks::BeforeNavigation, arena, ioTargetEntryId);
}

void BeforeTool(void* user, Arena* arena, StrView toolName, StrView toolCallId,
                const ToolArgs* args, AgentToolGateDecision* out) {
  const AgentHookChain* chain = (const AgentHookChain*)user;
  for (u32 i = 0; i < chain->Count; i++) {
    const AgentHarnessHooks& link = chain->Links[i];
    if (!link.BeforeTool) continue;
    AgentToolGateDecision decision = {};
    link.BeforeTool(link.User, arena, toolName, toolCallId, args, &decision);
    if (decision.Block) {
      *out = decision;
      return;
    }
  }
}

void AfterTool(void* user, Arena* arena, StrView toolName, StrView toolCallId,
               const ToolArgs* args, ToolOutcome* io) {
  const AgentHookChain* chain = (const AgentHookChain*)user;
  for (u32 i = 0; i < chain->Count; i++) {
    const AgentHarnessHooks& link = chain->Links[i];
    if (link.AfterTool)
      link.AfterTool(link.User, arena, toolName, toolCallId, args, io);
  }
}

SessionResult PrepareNextTurn(void* user, Arena* arena,
                              const AgentTurnContext* context,
                              AgentNextTurnUpdate* out) {
  const AgentHookChain* chain = (const AgentHookChain*)user;
  for (u32 i = 0; i < chain->Count; i++) {
    const AgentHarnessHooks& link = chain->Links[i];
    if (!link.PrepareNextTurn) continue;
    AgentNextTurnUpdate update = {};
    SessionResult result = link.PrepareNextTurn(link.User, arena, context, &update);
    if (!SessionResult_IsOk(result)) return result;
    if (update.HasContext) {
      out->HasContext = true;
      out->Context = update.Context;
    }
    if (update.HasModel) {
      out->HasModel = true;
      out->ModelInfo = update.ModelInfo;
    }
    if (update.HasProfile) {
      out->HasProfile = true;
      out->Profile = update.Profile;
    }
  }
  return SessionResult_Ok();
}

SessionResult ShouldStopAfterTurn(void* user, Arena* arena,
                                  const AgentTurnContext* context,
                                  bool* outStop) {
  const AgentHookChain* chain = (const AgentHookChain*)user;
  for (u32 i = 0; i < chain->Count && !*outStop; i++) {
    const AgentHarnessHooks& link = chain->Links[i];
    if (!link.ShouldStopAfterTurn) continue;
    SessionResult result = link.ShouldStopAfterTurn(link.User, arena, context, outStop);
    if (!SessionResult_IsOk(result)) return result;
  }
  return SessionResult_Ok();
}

SessionResult AfterTurn(void* user, Arena* arena, AgentTurnKind turn,
                        AgentTurnOutcome outcome, StrView error) {
  const AgentHookChain* chain = (const AgentHookChain*)user;
  SessionResult first = SessionResult_Ok();
  for (u32 i = 0; i < chain->Count; i++) {
    const AgentHarnessHooks& link = chain->Links[i];
    if (!link.AfterTurn) continue;
    SessionResult result = link.AfterTurn(link.User, arena, turn, outcome, error);
    if (SessionResult_IsOk(first) && !SessionResult_IsOk(result)) first = result;
  }
  return first;
}

}  // namespace

bool AgentHookChain_Add(AgentHookChain* chain, const AgentHarnessHooks* hooks) {
  if (!chain || !hooks || chain->Count >= AGENT_HOOK_CHAIN_CAPACITY) return false;
  chain->Links[chain->Count++] = *hooks;
  return true;
}

AgentHarnessHooks AgentHookChain_Compose(AgentHookChain* chain) {
  if (!chain || chain->Count == 0) return AgentHarnessHooks{};
  if (chain->Count == 1) return chain->Links[0];

  AgentHarnessHooks composed = {};
  composed.User = chain;
  if (AnyLink(chain, &AgentHarnessHooks::BeforeRun)) composed.BeforeRun = BeforeRun;
  if (AnyLink(chain, &AgentHarnessHooks::BeforeContext))
    composed.BeforeContext = BeforeContext;
  if (AnyLink(chain, &AgentHarnessHooks::ConvertToLlm))
    composed.ConvertToLlm = ConvertToLlm;
  if (AnyLink(chain, &AgentHarnessHooks::BeforeProvider))
    composed.BeforeProvider = BeforeProvider;
  if (AnyLink(chain, &AgentHarnessHooks::AfterProvider))
    composed.AfterProvider = AfterProvider;
  if (AnyLink(chain, &AgentHarnessHooks::RewriteToolArguments))
    composed.RewriteToolArguments = RewriteToolArguments;
  if (AnyLink(chain, &AgentHarnessHooks::BeforeTool)) composed.BeforeTool = BeforeTool;
  if (AnyLink(chain, &AgentHarnessHooks::AfterTool)) composed.AfterTool = AfterTool;
  if (AnyLink(chain, &AgentHarnessHooks::PrepareNextTurn))
    composed.PrepareNextTurn = PrepareNextTurn;
  if (AnyLink(chain, &AgentHarnessHooks::ShouldStopAfterTurn))
    composed.ShouldStopAfterTurn = ShouldStopAfterTurn;
  if (AnyLink(chain, &AgentHarnessHooks::GetSteeringMessages))
    composed.GetSteeringMessages = GetSteeringMessages;
  if (AnyLink(chain, &AgentHarnessHooks::GetFollowUpMessages))
    composed.GetFollowUpMessages = GetFollowUpMessages;
  if (AnyLink(chain, &AgentHarnessHooks::BeforeCompaction))
    composed.BeforeCompaction = BeforeCompaction;
  if (AnyLink(chain, &AgentHarnessHooks::BeforeNavigation))
    composed.BeforeNavigation = BeforeNavigation;
  if (AnyLink(chain, &AgentHarnessHooks::AfterTurn)) composed.AfterTurn = AfterTurn;
  return composed;
}
