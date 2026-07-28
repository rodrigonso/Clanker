#pragma once

#include "session/reducer.h"

struct AgentHarnessProviderInput {
  Model ModelInfo;
  LlmContext Context;
  StreamOptions Options;
};

struct AgentHarnessHooks {
  void* User;

  SessionResult (*BeforeRun)(void* user, Arena* arena, Message* ioPrompt);
  SessionResult (*BeforeContext)(void* user, Arena* arena,
                                 OperationKind operation,
                                 DurableId headId,
                                 Array<Message>* ioMessages);
  SessionResult (*BeforeProvider)(void* user, Arena* arena,
                                  OperationKind operation,
                                  u32 attemptIndex,
                                  AgentHarnessProviderInput* io);
  SessionResult (*AfterProvider)(void* user, Arena* arena,
                                 OperationKind operation,
                                 u32 attemptIndex,
                                 Message* ioAssistant);
  SessionResult (*BeforeTool)(void* user, Arena* arena,
                              StrView toolName,
                              StrView* ioArgumentsJson);
  SessionResult (*BeforeCompaction)(void* user, Arena* arena,
                                    DurableId headId);
  SessionResult (*BeforeNavigation)(void* user, Arena* arena,
                                    DurableId* ioTargetEntryId);
  SessionResult (*BeforeOperationComplete)(void* user, Arena* arena,
                                           OperationKind operation,
                                           OperationOutcome outcome,
                                           StrView error);
};
