#pragma once

#include "agent/harness.h"

SessionResult AgentDriver_StartRun(AgentHarness* harness,
                                   const Message* prompt,
                                   DurableId operationId,
                                   DurableId startingHeadId);
SessionResult AgentDriver_ResumeRun(AgentHarness* harness,
                                    const SessionReduction* restored);
SessionResult AgentDriver_Compact(AgentHarness* harness, DurableId operationId,
                                  DurableId startingHeadId,
                                  DurableId compactionEntryId);
SessionResult AgentDriver_ResumeCompaction(AgentHarness* harness,
                                           const SessionReduction* restored);
SessionResult AgentDriver_ResumeNavigation(AgentHarness* harness,
                                           const SessionReduction* restored);

// Internal durable append/load seams shared by the harness and driver.
SessionResult AgentHarness_Append(AgentHarness* harness, SessionRecord* records,
                                  u32 count);
SessionResult AgentHarness_Load(Arena* arena, AgentHarness* harness,
                                Array<SessionRecord>* records,
                                SessionReduction* reduction);
void AgentHarness_PublishTransient(AgentHarness* harness,
                                   DurableId operationId,
                                   DurableId attemptId,
                                   const StreamEvent* event);
