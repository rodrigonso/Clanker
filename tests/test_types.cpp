#include "agent/event.h"
#include "agent/tool.h"
#include "base/arena.h"
#include "check.h"
#include "llm/types.h"

void TestTypes() {
  Arena a = ArenaCreate();

  Message u = Message_User(&a, SV("hello"));
  CHECK(u.Kind == Role::User);
  CHECK(u.Content.Count == 1);
  CHECK(u.Content.Data[0].Kind == ContentKind::Text);
  CHECK(StrView_EqCStr(u.Content.Data[0].Text, "hello"));

  Message m = Message_AssistantEmpty(&a);
  Array_Push(&m.Content, Content_Text(SV("hi")));
  Array_Push(&m.Content, Content_ToolCall(SV("call_1"), SV("read"), SV("{\"path\":\"x\"}")));
  CHECK(m.Content.Count == 2);
  Content* tc = &m.Content.Data[1];
  CHECK(tc->Kind == ContentKind::ToolCall);
  CHECK(StrView_EqCStr(tc->ToolName, "read"));
  CHECK(StrView_EqCStr(tc->ToolArgsJson, "{\"path\":\"x\"}"));

  Message tr = Message_ToolResult(&a, SV("call_1"), SV("read"), SV("file contents"), false);
  CHECK(tr.Kind == Role::ToolResult);
  CHECK(!tr.IsError);
  CHECK(StrView_EqCStr(tr.ToolCallId, "call_1"));

  ArenaRelease(&a);
}
