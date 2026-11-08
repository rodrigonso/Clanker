#pragma once

#include "agent/ask.h"
#include "agent/tool.h"
#include "base/thread.h"
#include "llm/types.h"
#include "session/id.h"

enum class AgentPermissionMode : u8 { Ask, Auto, Yolo };

StrView AgentPermissionMode_Name(AgentPermissionMode mode);
bool AgentPermissionMode_Parse(StrView value, AgentPermissionMode* out);
AgentPermissionMode AgentPermissionMode_Next(AgentPermissionMode mode);

constexpr StrView AGENT_PERMISSION_DEFAULT_REVIEWER_MODEL =
    SVL("jev-latest");
constexpr f64 AGENT_PERMISSION_DEFAULT_REVIEWER_THRESHOLD = 0.95;

enum class AgentPermissionReviewDecision : u8 {
  Ask,
  Allow,
};

struct AgentPermissionReviewRequest {
  StrView ToolName;
  StrView Action;
  StrView TrustedUserText;
  bool ActionIncomplete;
  bool TrustedUserTextIncomplete;
};

struct AgentPermissionReviewResult {
  AgentPermissionReviewDecision Decision;
  StrView Rationale;
  StrView Model;
};

typedef bool (*AgentPermissionReviewFn)(
    void* user, Arena* arena, const AgentPermissionReviewRequest* request,
    volatile i32* abort, AgentPermissionReviewResult* out);

struct AgentPermissionReviewer {
  void* User;
  AgentPermissionReviewFn Review;
};

struct AgentPermissionGrant {
  StrView Key;
};

enum class AgentPermissionApproval : u8 {
  Pending,
  Once,
  Denied,
  Consumed,
};

struct AgentPermissionRequest {
  DurableId Id;
  StrView ToolName;
  StrView ArgumentsJson;
  StrView GrantKey;
  StrView Action;
  StrView Rationale;
  AgentPermissionApproval Approval;
};

struct AgentPermissionCacheEntry {
  StrView Key;
  bool Allow;
  u32 RequestIndex;
  i32 Revision;
  u32 ContextRevision;
};

struct AgentPermissionEngine {
  Arena Session;
  Arena GrantMem;
  Arena Run;
  // Holds WritableRoots; reset whenever they are replaced.
  Arena RootMem;
  Mutex Lock;
  const AgentAskUser* AskUser;
  AgentPermissionReviewer Reviewer;
  StrView WorkingDirectory;
  Array<StrView> WritableRoots;
  Array<AgentPermissionGrant> Grants;
  Array<AgentPermissionRequest> Requests;
  Array<AgentPermissionCacheEntry> Cache;
  StrBuf ReviewContext;
  bool ReviewContextIncomplete;
  u32 ContextRevision;
  volatile i32 Mode;
  volatile i32 Revision;
  bool Initialized;
  bool RunActive;
};

struct AgentPermissionGate {
  AgentPermissionEngine* Engine;
  const Message* NewMessages;
  u32 NewMessageCount;
  volatile i32* Abort;
};

struct AgentPermissionDecision {
  bool Block;
  StrView Reason;
};

void AgentPermissionEngine_Init(AgentPermissionEngine* engine,
                                AgentPermissionMode mode,
                                StrView workingDirectory,
                                const AgentAskUser* askUser,
                                AgentPermissionReviewer reviewer);
void AgentPermissionEngine_Close(AgentPermissionEngine* engine);
// Replaces the directories outside the working directory that file mutations
// may target as freely as the working directory itself. Copied.
void AgentPermissionEngine_SetWritableRoots(AgentPermissionEngine* engine,
                                            const StrView* roots, u32 count);
void AgentPermissionEngine_BeginRun(AgentPermissionEngine* engine);
void AgentPermissionEngine_EndRun(AgentPermissionEngine* engine);
void AgentPermissionEngine_InvalidateReviews(AgentPermissionEngine* engine);

AgentPermissionMode AgentPermissionEngine_GetMode(
    const AgentPermissionEngine* engine);
AgentPermissionMode AgentPermissionEngine_SetMode(AgentPermissionEngine* engine,
                                                  AgentPermissionMode mode);
AgentPermissionMode AgentPermissionEngine_CycleMode(
    AgentPermissionEngine* engine);
AgentPermissionMode AgentPermissionEngine_Reset(AgentPermissionEngine* engine);
u32 AgentPermissionEngine_GrantCount(AgentPermissionEngine* engine);

void AgentPermission_Decide(Arena* arena, const AgentPermissionGate* gate,
                            const Tool* tool, const ToolArgs* args,
                            AgentPermissionDecision* out);

bool AgentPermission_ResolveRequest(AgentPermissionEngine* engine, Arena* arena,
                                    StrView requestId, volatile i32* abort,
                                    StrView* outMessage, StrBuf* error);

StrView AgentPermission_AppendSystemPrompt(Arena* arena, StrView base,
                                           AgentPermissionMode mode);
