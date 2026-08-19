#pragma once
#include "base/basics.h"
#include "base/str.h"
#include "llm/types.h"

enum { TUI_PERF_WINDOW = 64 };
enum { TUI_PERF_TOOL_CAP = 14 };

struct TuiPerf {
  u64 LastFrameUs;
  u64 TurnStartUs;
  u64 ToolStartUs;
  u64 RssBytes;
  u64 LastRssSampleMs;
  u32 Window[TUI_PERF_WINDOW];
  struct {
    u32 Input;
    u32 Output;
    u32 CacheRead;
  } LastTokens;
  u32 LastTtftUs;
  u32 LastTurnUs;
  u32 LastToolUs;
  u32 DroppedEvents;
  char LastTool[TUI_PERF_TOOL_CAP];
  u8 Head;
  u8 Count;
  u8 LastToolLength;
  bool Visible;
};

void TuiPerf_FrameRendered(TuiPerf* p, u64 nowUs);
void TuiPerf_TurnStarted(TuiPerf* p, u64 nowUs);
void TuiPerf_AssistantDelta(TuiPerf* p, u64 nowUs);
void TuiPerf_TurnMessageEnded(TuiPerf* p, Usage use, u64 nowUs);
void TuiPerf_ToolStarted(TuiPerf* p, StrView name, u64 nowUs);
void TuiPerf_ToolEnded(TuiPerf* p, u64 nowUs);
