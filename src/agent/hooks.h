#pragma once

#include "session/reducer.h"

// Distinguishes the two provider-driving turn kinds the harness can run.
// Navigation (checkout) never calls the provider, so it only needs
// BeforeNavigation below, not a turn kind of its own.
enum class AgentTurnKind : u8 { Run, Compaction };

// Purely transient: turns are no longer durable Operation records, so this
// is never persisted, only reported to hooks as each turn ends.
enum class AgentTurnOutcome : u8 { Succeeded, Failed, Aborted };

struct AgentHarnessProviderInput {
  Model ModelInfo;
  LlmContext Context;
  StreamOptions Options;
};

struct AgentHarnessHooks {
  void* User;

  SessionResult (*BeforeRun)(void* user, Arena* arena, Message* ioPrompt);
  SessionResult (*BeforeContext)(void* user, Arena* arena,
                                 AgentTurnKind turn,
                                 DurableId headId,
                                 Array<Message>* ioMessages);
  SessionResult (*BeforeProvider)(void* user, Arena* arena,
                                  AgentTurnKind turn,
                                  u32 attemptIndex,
                                  AgentHarnessProviderInput* io);
  SessionResult (*AfterProvider)(void* user, Arena* arena,
                                 AgentTurnKind turn,
                                 u32 attemptIndex,
                                 Message* ioAssistant);
  SessionResult (*BeforeTool)(void* user, Arena* arena,
                              StrView toolName,
                              StrView* ioArgumentsJson);
  SessionResult (*BeforeCompaction)(void* user, Arena* arena,
                                    DurableId headId);
  SessionResult (*BeforeNavigation)(void* user, Arena* arena,
                                    DurableId* ioTargetEntryId);
  SessionResult (*AfterTurn)(void* user, Arena* arena,
                             AgentTurnKind turn,
                             AgentTurnOutcome outcome,
                             StrView error);
};
