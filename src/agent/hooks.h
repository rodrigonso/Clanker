#pragma once

#include "agent/tool.h"
#include "session/reducer.h"

// A finished tool call as the after-tool hook sees and may rewrite it.
struct ToolOutcome {
  ContentArray Content;
  bool IsError;
  bool Terminate;
};

// Verdict of the before-tool gate, mirroring pi's `BeforeToolCallResult`.
// Zero-initialized means "allow". `Reason` becomes the blocked call's tool
// result verbatim; an empty one falls back to AGENT_TOOL_BLOCKED_FALLBACK.
// `Terminate` only ends the run when every call in the batch sets it, which is
// what AgentToolBatchResult::AllTerminate already computes.
struct AgentToolGateDecision {
  bool Block;
  StrView Reason;
  bool Terminate;
};

// pi's wording for a block that supplied no reason, byte for byte. Both are
// model-visible, so neither may be reworded casually.
constexpr StrView AGENT_TOOL_BLOCKED_FALLBACK = SVL("Tool execution was blocked");

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
// arena-backed by turn scratch and only valid for the duration of the call.
// Nested arrays/text are detached from durable and override storage. Changes
// to this observational input are not overrides; return AgentNextTurnUpdate.
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
// Accepted context/model fields are copied into replaceable per-run owners;
// hook construction allocations are only valid until the end of this turn.
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

  // Callback inputs and construction arena are turn-scoped, not retained
  // session storage. Mutable messages include owned nested Content arrays
  // (and their Backing) and text; provider inputs also detach model, system,
  // schema and option views. Borrowed replacement fields must remain readable
  // through this turn; the driver detaches them before another mutable hook
  // and copies accepted durable/override data into its destination owner.
  // Request rewrites never modify durable history or active overrides.
  // Opaque callback/user pointers and the live cancellation signal are not
  // mutable configuration; hooks must not modify their pointees.
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
  // Rewrites one tool call's raw argument JSON before it is parsed and
  // validated. The rewrite is what the session records as the call.
  SessionResult (*RewriteToolArguments)(void* user, Arena* arena,
                                        StrView toolName,
                                        StrView* ioArgumentsJson);
  // pi's `beforeToolCall`. Runs during preparation, after argument
  // normalization and schema validation, on the coordinator thread and
  // serially across the batch -- pi's clearance phase. A gate may block on
  // external work; nothing in the batch has started yet.
  void (*BeforeTool)(void* user, Arena* arena, StrView toolName,
                     StrView toolCallId, const ToolArgs* args,
                     AgentToolGateDecision* out);
  // pi's `afterToolCall`. Runs on the coordinator thread once a call finishes,
  // before its result is durable; rewriting `io` changes what is recorded.
  void (*AfterTool)(void* user, Arena* arena, StrView toolName,
                    StrView toolCallId, const ToolArgs* args, ToolOutcome* io);
  // Runs after a run turn's `turn_end`, before the loop decides whether
  // another provider request should start. Writes replacement model / effort /
  // context for the rest of this run. The arena is turn-scoped scratch; the
  // harness copies accepted fields before recycling it. pi: `prepareNextTurn`.
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
