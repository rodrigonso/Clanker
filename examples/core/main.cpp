#include <stdio.h>

#include "agent/harness.h"
#include "session/memory_repository.h"

namespace {

bool LocalProvider(void*, Arena* arena, const Model* model, const LlmContext*,
                   const StreamOptions* options, StreamSink sink, Message* out) {
  *out = Message_AssistantEmpty(arena);
  out->Model = model->Id;
  out->Provider = model->Provider;
  if (options->Abort && Atomic_LoadI32(options->Abort)) {
    out->Stop = StopReason::Aborted;
    out->ErrorMessage = SVL("aborted");
    StreamSink_Emit(sink, StreamEvent{StreamEventKind::Error, 0, {}});
    return false;
  }
  StrView text = SVL("Hello from the dependency-free Clanker core.");
  Array_Push(&out->Content, Content_Text(StrView_Dup(arena, text)));
  out->Stop = StopReason::Stop;
  StreamSink_Emit(sink, StreamEvent{StreamEventKind::Start, 0, {}});
  StreamSink_Emit(sink, StreamEvent{StreamEventKind::TextStart, 0, {}});
  StreamSink_Emit(sink, StreamEvent{StreamEventKind::TextDelta, 0, text});
  StreamSink_Emit(sink, StreamEvent{StreamEventKind::TextEnd, 0, {}});
  StreamSink_Emit(sink, StreamEvent{StreamEventKind::Done, 0, {}});
  return true;
}

bool Report(SessionResult result) {
  if (SessionResult_IsOk(result)) return true;
  fprintf(stderr, "error: %.*s\n", (int)result.Message.Length,
          result.Message.Data ? result.Message.Data : "");
  return false;
}

}  // namespace

int main() {
  Arena arena = ArenaCreate();
  MemoryRepository memory = {};
  MemoryRepository_Init(&memory);
  SessionRepository repository = MemoryRepository_AsRepository(&memory);
  SessionCreateOptions create = {};
  create.ProjectKey = SVL("core-example");
  create.WorkingDirectory = SVL("example");
  SessionLog log = {};
  SessionInfo info = {};
  if (!Report(repository.Create(repository.Ctx, &arena, &create, &log, &info))) {
    MemoryRepository_Destroy(&memory);
    ArenaRelease(&arena);
    return 1;
  }

  AgentHarnessConfig config = {};
  config.ModelInfo.Id = SVL("local-example");
  config.ModelInfo.Provider = SVL("local");
  config.ProviderImpl.Stream = LocalProvider;
  config.PermissionMode = AgentPermissionMode::Ask;
  AgentHarness harness = {};
  // Init takes ownership of the log; config and provider context outlive Run.
  bool ok = Report(AgentHarness_Init(&harness, log, &config));
  if (ok) ok = Report(AgentHarness_Run(&harness, SVL("Say hello")));
  SessionReduction snapshot = {};
  Array<Message> messages = {};
  if (ok) ok = Report(AgentHarness_Snapshot(&arena, &harness, &snapshot));
  if (ok) ok = Report(SessionReducer_Context(&arena, &snapshot,
                                             snapshot.HeadId, &messages));
  if (ok) {
    for (u32 i = 0; i < messages.Count; i++) {
      const Message& message = messages.Data[i];
      if (message.Kind != Role::Assistant) continue;
      for (u32 j = 0; j < message.Content.Count; j++) {
        const Content& content = message.Content.Data[j];
        if (content.Kind == ContentKind::Text)
          printf("%.*s\n", (int)content.Text.Length, content.Text.Data);
      }
    }
  }
  if (harness.Initialized && !Report(AgentHarness_Close(&harness))) ok = false;
  MemoryRepository_Destroy(&memory);
  ArenaRelease(&arena);
  return ok ? 0 : 1;
}
