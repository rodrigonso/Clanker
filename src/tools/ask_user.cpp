#include "agent/ask.h"
#include "agent/permissions.h"
#include "base/thread.h"
#include "json/json.h"
#include "tools/tools.h"

// Bounded so a malformed call cannot ask the user to pick from an unusable
// list. Well above anything a real question needs.
static constexpr u32 ASK_USER_MAX_CHOICES = 16;

// Every schema property is optional and nullable, so a model that fills the
// unused half of the call with explicit nulls still clears argument validation.
// A null is "not supplied"; the real exclusivity is enforced below, where it
// can be explained.
static const JsonValue* AskUserArg(const ToolArgs* args, const char* key) {
  const JsonValue* value = ToolArgs_Get(args, key);
  return value && value->Kind != JsonKind::Null ? value : nullptr;
}

static bool AskUserExec(void* ctx, Arena* arena, StrView id, const ToolArgs* args,
                        volatile i32* abort, const ToolUpdateSink* updates,
                        ToolResult* out, StrBuf* err) {
  (void)id;
  (void)updates;

  const AskUserToolContext* context = (const AskUserToolContext*)ctx;
  const JsonValue* permissionIdValue =
      AskUserArg(args, "permission_request_id");
  if (permissionIdValue) {
    if (!Json_IsString(permissionIdValue) ||
        permissionIdValue->Str.Length == 0) {
      StrBuf_AppendCStr(err, "'permission_request_id' must be a non-empty string");
      return false;
    }
    if (AskUserArg(args, "question") || AskUserArg(args, "choices")) {
      StrBuf_AppendCStr(
          err, "'permission_request_id' is mutually exclusive with 'question' "
               "and 'choices'");
      return false;
    }

    if (!context || !context->Permissions) {
      StrBuf_AppendCStr(err,
                        "no permission engine is available to resolve this "
                        "request");
      return false;
    }
    StrView message = {};
    if (!AgentPermission_ResolveRequest(
            context->Permissions, arena, permissionIdValue->Str, abort,
            &message, err))
      return false;
    out->Content = Array_Make<Content>(arena, 1);
    Array_Push(&out->Content, Content_Text(message));
    return true;
  }

  StrView question = ToolArgs_GetStr(args, "question");
  if (question.Length == 0) {
    StrBuf_AppendCStr(err,
                      "missing 'question' argument; to answer a denied tool "
                      "call, pass 'permission_request_id' instead");
    return false;
  }

  StrView choices[ASK_USER_MAX_CHOICES] = {};
  u32 choiceCount = 0;
  const JsonValue* rawChoices = AskUserArg(args, "choices");
  if (rawChoices) {
    if (!Json_IsArray(rawChoices)) {
      StrBuf_AppendCStr(err, "'choices' must be an array of strings");
      return false;
    }
    if (rawChoices->Count > ASK_USER_MAX_CHOICES) {
      StrBuf_AppendCStr(err, "'choices' accepts at most ");
      StrBuf_AppendU64(err, ASK_USER_MAX_CHOICES);
      StrBuf_AppendCStr(err, " options");
      return false;
    }
    for (u32 i = 0; i < rawChoices->Count; i++) {
      const JsonValue* entry = rawChoices->Items[i];
      if (!Json_IsString(entry) || entry->Str.Length == 0) {
        StrBuf_AppendCStr(err, "'choices' must contain only non-empty strings");
        return false;
      }
      choices[choiceCount++] = entry->Str;
    }
  }

  // Asking would block indefinitely on a run that is already being torn down.
  if (abort && Atomic_LoadI32(abort)) {
    StrBuf_AppendCStr(err, "cancelled before the question could be asked");
    return false;
  }

  AgentAskRequest request = {};
  request.Question = question;
  request.Choices = choiceCount ? choices : nullptr;
  request.ChoiceCount = choiceCount;
  AgentAskResponse response =
      AgentAskUser_Invoke(context ? &context->Ask : nullptr, arena, &request);

  if (response.Status == AgentAskStatus::Unavailable) {
    // An error result rather than a fastfail: a headless run should be able to
    // continue and work the problem out on its own, and the model can only do
    // that if it is told why the question went unanswered.
    StrBuf_AppendCStr(err,
                      "no user is available to answer questions in this "
                      "session; proceed without asking");
    return false;
  }
  if (response.Status == AgentAskStatus::Declined) {
    StrBuf_AppendCStr(err, "the user dismissed the question without answering");
    return false;
  }

  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(response.Answer));
  return true;
}

constexpr StrView kAskUserPromptGuidelines[] = {
    SVL("Use ask_user only when guessing wrong would waste significant work; "
        "otherwise pick the reasonable default and say what you chose.")};

Tool AskUserTool(AskUserToolContext* context) {
  Tool t = {};
  t.Name = SVL("ask_user");
  t.Label = SVL("Ask user");
  t.Description = SVL("Ask the user a question and wait for their answer.");
  t.PromptSnippet = SVL("Ask the user a question when a decision is ambiguous");
  Tool_SetPromptGuidelines(&t, kAskUserPromptGuidelines);
  t.ParamsJsonSchema = SVL(
      "{\"type\":\"object\",\"properties\":{\"question\":{\"type\":[\"string\","
      "\"null\"],\"description\":\"The question to put to the user; required "
      "unless permission_request_id is set\"},\"choices\":{\"type\":[\"array\","
      "\"null\"],\"items\":{\"type\":\"string\"},\"description\":\"Optional fixed "
      "answers to choose from; omit to ask for free-form text\"},"
      "\"permission_request_id\":{\"type\":[\"string\",\"null\"],\"description\":"
      "\"Permission request ID returned by a denied tool call; send it on its "
      "own, with no question or choices\"}},\"additionalProperties\":false}");
  // A batch runs in parallel by default, and there is only one user and one
  // modal: two concurrent questions would race for it.
  t.ExecMode = ToolExecMode::Sequential;
  t.PermissionEffect = ToolPermissionEffect::UserInteraction;
  t.Ctx = context;
  t.Execute = AskUserExec;
  return t;
}
