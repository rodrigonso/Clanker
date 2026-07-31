#pragma once

#include "base/array.h"
#include "base/basics.h"
#include "llm/types.h"

// Pi-style message normalization performed only while building a provider
// request. It never mutates or persists the session log:
//  - Assistant messages that ended in an error or were aborted are omitted
//    from replay (a later run naturally continues the branch without ever
//    sending the failed turn back to the provider).
//  - Any assistant tool call left without a corresponding tool-result message
//    (for example after a process crash) gets a synthetic, transient error
//    tool result containing "No result provided" inserted where the real
//    result would have appeared. Synthetic results are never written back to
//    the session log or treated as having run; a future implementation is
//    free to change this transform without touching any stored branch.
inline StrView SessionReplay_NoResultMessage() {
  return StrView{"No result provided", 18};
}

inline bool SessionReplay_ShouldReplayAssistant(const Message& message) {
  return message.Kind != Role::Assistant ||
         (message.Stop != StopReason::Error &&
          message.Stop != StopReason::Aborted);
}

inline Message SessionReplay_MakeNoResult(Arena* arena, StrView toolCallId,
                                          StrView toolName, u64 timestamp) {
  Message synthetic = {};
  synthetic.Kind = Role::ToolResult;
  synthetic.ToolCallId = toolCallId;
  synthetic.ToolName = toolName;
  synthetic.IsError = true;
  synthetic.Timestamp = timestamp;
  synthetic.Content = Array_Make<Content>(arena, 1);
  Array_Push(&synthetic.Content, Content_Text(SessionReplay_NoResultMessage()));
  return synthetic;
}

inline Array<Message> SessionReplay_TransformMessages(Arena* arena,
                                                      const Message* messages,
                                                      u32 count) {
  Array<Message> result = Array_Make<Message>(arena, count ? count : 1);
  Array<StrView> pendingIds = Array_Make<StrView>(arena, 4);
  Array<StrView> pendingNames = Array_Make<StrView>(arena, 4);
  u64 lastTimestamp = 0;

  for (u32 i = 0; i < count; i++) {
    const Message& message = messages[i];
    if (message.Kind == Role::ToolResult) {
      for (u32 p = 0; p < pendingIds.Count; p++) {
        if (StrView_Eq(pendingIds.Data[p], message.ToolCallId)) {
          pendingIds.Data[p] = pendingIds.Data[pendingIds.Count - 1];
          pendingNames.Data[p] = pendingNames.Data[pendingNames.Count - 1];
          pendingIds.Count--;
          pendingNames.Count--;
          break;
        }
      }
      Array_Push(&result, message);
      lastTimestamp = message.Timestamp;
      continue;
    }

    // A user or assistant message ends the run of tool results that could
    // satisfy the previous assistant's tool calls; synthesize whatever is
    // still missing before moving on to the new message.
    for (u32 p = 0; p < pendingIds.Count; p++)
      Array_Push(&result, SessionReplay_MakeNoResult(
                              arena, pendingIds.Data[p], pendingNames.Data[p],
                              lastTimestamp));
    pendingIds.Count = 0;
    pendingNames.Count = 0;

    if (!SessionReplay_ShouldReplayAssistant(message)) continue;
    Array_Push(&result, message);
    lastTimestamp = message.Timestamp;
    if (message.Kind == Role::Assistant) {
      for (u32 c = 0; c < message.Content.Count; c++) {
        if (message.Content.Data[c].Kind == ContentKind::ToolCall) {
          Array_Push(&pendingIds, message.Content.Data[c].ToolId);
          Array_Push(&pendingNames, message.Content.Data[c].ToolName);
        }
      }
    }
  }
  for (u32 p = 0; p < pendingIds.Count; p++)
    Array_Push(&result, SessionReplay_MakeNoResult(arena, pendingIds.Data[p],
                                                   pendingNames.Data[p],
                                                   lastTimestamp));
  return result;
}
