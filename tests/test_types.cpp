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

  Model model = {};
  model.ContextWindow = 200000;
  model.ContextWindowOptions[0] = 200000;
  model.ContextWindowOptions[1] = 1000000;
  model.ContextWindowOptionCount = 2;
  model.EffortMask = ModelEffort_Bit(ModelEffort::Low) |
                     ModelEffort_Bit(ModelEffort::High);
  model.DefaultEffort = ModelEffort::High;
  CHECK_ALL(Model_DefaultEffort(&model) == ModelEffort::High,
            Model_EffortOptionCount(&model) == 2,
            Model_SupportsEffort(&model, ModelEffort::Low),
            !Model_SupportsEffort(&model, ModelEffort::Medium),
            Model_SupportsContextWindow(&model, 1000000),
            !Model_SupportsContextWindow(&model, 500000),
            Model_MetadataIsValid(&model));
  ModelRequestProfile resolved =
      Model_ResolveRequestProfile(&model, ModelRequestProfile{});
  CHECK_ALL(resolved.Effort == ModelEffort::High,
            resolved.ContextWindow == 200000,
            Model_RequestProfileIsValid(
                &model,
                ModelRequestProfile{ModelEffort::Low, 1000000}),
            !Model_RequestProfileIsValid(
                &model,
                ModelRequestProfile{ModelEffort::Medium, 200000}));
  bool unsupported = false;
  ModelRequestProfile normalized = Model_NormalizeRequestPreference(
      &model, ModelRequestProfile{ModelEffort::Low, 1000000}, &unsupported);
  CHECK_ALL(!unsupported, normalized.Effort == ModelEffort::Low,
            normalized.ContextWindow == 1000000);
  normalized = Model_NormalizeRequestPreference(
      &model, ModelRequestProfile{ModelEffort::Medium, 500000}, &unsupported);
  CHECK_ALL(unsupported, normalized.Effort == ModelEffort::None,
            normalized.ContextWindow == 0);

  Model fixed = model;
  fixed.ContextWindowOptionCount = 0;
  normalized = Model_NormalizeRequestPreference(
      &fixed, ModelRequestProfile{ModelEffort::Low, 200000}, &unsupported);
  CHECK_ALL(!unsupported, normalized.Effort == ModelEffort::Low,
            normalized.ContextWindow == 0);

  Model fixedEffort = model;
  fixedEffort.EffortMask = ModelEffort_Bit(ModelEffort::High);
  normalized = Model_NormalizeRequestPreference(
      &fixedEffort, ModelRequestProfile{ModelEffort::High, 1000000},
      &unsupported);
  CHECK_ALL(!unsupported, normalized.Effort == ModelEffort::None,
            normalized.ContextWindow == 1000000);

  Model invalid = model;
  invalid.DefaultEffort = ModelEffort::Medium;
  CHECK(!Model_MetadataIsValid(&invalid));
  invalid = model;
  invalid.ContextWindowOptions[1] = invalid.ContextWindowOptions[0];
  CHECK(!Model_MetadataIsValid(&invalid));

  char contextLabel[MODEL_CONTEXT_WINDOW_LABEL_CAPACITY] = {};
  CHECK(ModelContextWindow_Format(contextLabel, sizeof(contextLabel),
                                  1000000) == 2);
  CHECK(StrView_EqCStr(StrView{contextLabel, 2}, "1M"));
  CHECK(ModelContextWindow_Format(contextLabel, sizeof(contextLabel),
                                  200000) == 4);
  CHECK(StrView_EqCStr(StrView{contextLabel, 4}, "200K"));
  ModelEffort parsed = ModelEffort::None;
  CHECK(ModelEffort_Parse(SVL("xhigh"), &parsed));
  CHECK(parsed == ModelEffort::XHigh);
  CHECK(!ModelEffort_Parse(SVL("extreme"), &parsed));

  ArenaRelease(&a);
}
