#pragma once

#include "base/thread.h"
#include "session/reducer.h"

struct AgentHarness;

constexpr u32 AGENT_HARNESS_MAX_WATCHERS = 8;
constexpr u32 AGENT_HARNESS_MAX_WATCH_EVENTS = 256;

enum class AgentHarnessEventKind : u8 {
  DurableRecord,
  TransientStream,
};

struct AgentHarnessEvent {
  AgentHarnessEventKind Kind;
  u64 Cursor;
  DurableId OperationId;
  DurableId AttemptId;
  SessionRecord Record;
  StreamEvent Stream;
};

struct AgentHarnessSnapshot {
  SessionReduction State;
  Array<Message> Transcript;
  u64 Cursor;
  bool Running;
  bool Suspended;
  bool Faulted;
};

struct AgentHarnessWatchSlot {
  Arena Mem;
  AgentHarnessEvent Event;
};

struct AgentHarnessWatch {
  AgentHarness* Owner;
  DurableId Id;
  Mutex Lock;
  CondVar Changed;
  AgentHarnessWatchSlot Slots[AGENT_HARNESS_MAX_WATCH_EVENTS];
  u32 Capacity;
  u32 Head;
  u32 Count;
  u64 Cursor;
  bool Initialized;
  bool Registered;
  bool Lagged;
  bool Closed;
  u32 Waiting;
};
