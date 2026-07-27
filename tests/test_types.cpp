#include "agent/event.h"
#include "agent/tool.h"
#include "base/arena.h"
#include "check.h"
#include "llm/types.h"

void TestTypes() {
  Arena a = ArenaCreate();

  Message u = Message_User(&a, SV("hello"));
  CHECK_ALL(u.Kind == Role::User, u.Content.Count == 1,
            u.Content.Data[0].Kind == ContentKind::Text,
            StrView_EqCStr(u.Content.Data[0].Text, "hello"));

  Message m = Message_AssistantEmpty(&a);
  Array_Push(&m.Content, Content_Text(SV("hi")));
  Array_Push(&m.Content, Content_ToolCall(SV("call_1"), SV("read"), SV("{\"path\":\"x\"}")));
  Content* tc = &m.Content.Data[1];
  CHECK_ALL(m.Content.Count == 2, tc->Kind == ContentKind::ToolCall,
            StrView_EqCStr(tc->ToolName, "read"),
            StrView_EqCStr(tc->ToolArgsJson, "{\"path\":\"x\"}"));

  Message tr = Message_ToolResult(&a, SV("call_1"), SV("read"), SV("file contents"), false);
  CHECK_ALL(tr.Kind == Role::ToolResult, !tr.IsError,
            StrView_EqCStr(tr.ToolCallId, "call_1"));

  ArenaRelease(&a);
}
