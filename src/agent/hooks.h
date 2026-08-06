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

// State handed to the per-turn decision hooks once a run turn's assistant
// message and tool results are durable, mirroring pi's
// ShouldStopAfterTurnContext / PrepareNextTurnContext. Every array is
// arena-backed by the harness and only valid for the duration of the call.
struct AgentTurnContext {
  AgentTurnKind Turn;
  DurableId HeadId;
  const Message* Assistant;
  Array<Message> ToolResults;
  // Model-visible branch context after the assistant message and tool results
  // were appended.
  Array<Message> Context;
  // Every message this run has appended so far, in append order.
  Array<Message> NewMessages;
};

// Replacement runtime state for the remaining turns of the current run,
// covering pi's AgentLoopTurnUpdate. Zero-initialized means "change nothing";
// each Has* flag opts a single field in. Overrides never touch the session
// tree or the harness config -- they expire when the run ends.
//
// A replacement Context is copied and then kept live: every message the run
// appends afterwards (assistant, tool results, steering, follow-up) is
// accumulated onto it, exactly as pi grows its currentContext. Automatic
// compaction sizes itself against the override rather than the branch it
// replaced -- by byte estimate, since a hook may keep a billed assistant
// message while dropping the history that usage paid for -- so a deliberately
// trimmed context is never folded away by history it already discarded. If the
// override itself outgrows the window, compaction folds the branch and drops
// the override, which cannot be rebased onto the folded branch.
struct AgentNextTurnUpdate {
  bool HasContext;
  Array<Message> Context;
  bool HasModel;
  Model ModelInfo;
  bool HasProfile;
  ModelRequestProfile Profile;
};

struct AgentHarnessHooks {
  void* User;

  SessionResult (*BeforeRun)(void* user, Arena* arena, Message* ioPrompt);
  SessionResult (*BeforeContext)(void* user, Arena* arena,
                                 AgentTurnKind turn,
                                 DurableId headId,
                                 Array<Message>* ioMessages);
  // Last rewrite of the message array before it is turned into a provider
  // request, running after BeforeContext. Covers pi's `convertToLlm`: drop or
  // rewrite anything that must not reach the model.
  SessionResult (*ConvertToLlm)(void* user, Arena* arena,
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
  // Runs after a run turn's `turn_end`, before the loop decides whether
  // another provider request should start. Writes replacement model / effort /
  // context for the rest of this run. pi: `prepareNextTurn`.
  SessionResult (*PrepareNextTurn)(void* user, Arena* arena,
                                   const AgentTurnContext* context,
                                   AgentNextTurnUpdate* out);
  // Runs immediately after PrepareNextTurn. Setting *outStop ends the run
  // normally, without polling the steering or follow-up sources.
  // pi: `shouldStopAfterTurn`.
  SessionResult (*ShouldStopAfterTurn)(void* user, Arena* arena,
                                       const AgentTurnContext* context,
                                       bool* outStop);
  // Message sources polled alongside the harness's own in-memory queues, at
  // the same two checkpoints: steering before the run's first turn and after
  // every tool-result batch, follow-up when the run would otherwise stop.
  // Returned messages are appended to the branch in order.
  // pi: `getSteeringMessages` / `getFollowUpMessages`.
  SessionResult (*GetSteeringMessages)(void* user, Arena* arena,
                                       Array<Message>* outMessages);
  SessionResult (*GetFollowUpMessages)(void* user, Arena* arena,
                                       Array<Message>* outMessages);
  SessionResult (*BeforeCompaction)(void* user, Arena* arena,
                                    DurableId headId);
  SessionResult (*BeforeNavigation)(void* user, Arena* arena,
                                    DurableId* ioTargetEntryId);
  SessionResult (*AfterTurn)(void* user, Arena* arena,
                             AgentTurnKind turn,
                             AgentTurnOutcome outcome,
                             StrView error);
};
