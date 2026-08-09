#include "base/thread.h"

#include <windows.h>

static_assert(sizeof(HANDLE) <= 16, "Thread storage too small");
static_assert(sizeof(CRITICAL_SECTION) <= 64, "Mutex storage too small");
static_assert(sizeof(CONDITION_VARIABLE) <= 64, "CondVar storage too small");

static DWORD WINAPI Trampoline(LPVOID p) {
  Thread* t = (Thread*)p;
  t->Fn(t->Arg);
  return 0;
}

static void CALLBACK PoolTrampoline(PTP_CALLBACK_INSTANCE, void* context) {
  ThreadPoolTask* task = (ThreadPoolTask*)context;
  task->Fn(task->Arg);
}

bool Thread_Start(Thread* t, ThreadFn fn, void* arg) {
  t->Fn = fn;
  t->Arg = arg;
  HANDLE* h = (HANDLE*)t->Storage;
  *h = CreateThread(nullptr, 0, Trampoline, t, 0, nullptr);
  return *h != nullptr;
}

bool ThreadPool_Submit(ThreadPoolTask* task, ThreadFn fn, void* arg) {
  task->Fn = fn;
  task->Arg = arg;
  return TrySubmitThreadpoolCallback(PoolTrampoline, task, nullptr) != FALSE;
}

void Thread_Join(Thread* t) {
  HANDLE* h = (HANDLE*)t->Storage;
  if (*h) {
    WaitForSingleObject(*h, INFINITE);
    CloseHandle(*h);
    *h = nullptr;
  }
}

void Mutex_Init(Mutex* m) { InitializeCriticalSection((CRITICAL_SECTION*)m->Storage); }
void Mutex_Destroy(Mutex* m) { DeleteCriticalSection((CRITICAL_SECTION*)m->Storage); }
void Mutex_Lock(Mutex* m) { EnterCriticalSection((CRITICAL_SECTION*)m->Storage); }
void Mutex_Unlock(Mutex* m) { LeaveCriticalSection((CRITICAL_SECTION*)m->Storage); }

void Cond_Init(CondVar* c) { InitializeConditionVariable((CONDITION_VARIABLE*)c->Storage); }
void Cond_Destroy(CondVar*) {}  // CONDITION_VARIABLE needs no teardown
void Cond_Wait(CondVar* c, Mutex* m) {
  SleepConditionVariableCS((CONDITION_VARIABLE*)c->Storage, (CRITICAL_SECTION*)m->Storage, INFINITE);
}
void Cond_WaitTimeout(CondVar* c, Mutex* m, u32 ms) {
  SleepConditionVariableCS((CONDITION_VARIABLE*)c->Storage, (CRITICAL_SECTION*)m->Storage, ms);
}
void Cond_Signal(CondVar* c) { WakeConditionVariable((CONDITION_VARIABLE*)c->Storage); }
void Cond_Broadcast(CondVar* c) { WakeAllConditionVariable((CONDITION_VARIABLE*)c->Storage); }

void Sleep_Ms(u32 ms) { Sleep(ms); }

u64 MonotonicMillis() { return GetTickCount64(); }

static volatile LONGLONG g_qpcFrequency = 0;

u64 MonotonicMicros() {
  LONGLONG frequency =
      InterlockedCompareExchange64(&g_qpcFrequency, 0, 0);
  if (frequency == 0) {
    LARGE_INTEGER measured;
    FFI(!QueryPerformanceFrequency(&measured) || measured.QuadPart <= 0);
    frequency = measured.QuadPart;
    LONGLONG previous =
        InterlockedCompareExchange64(&g_qpcFrequency, frequency, 0);
    if (previous) frequency = previous;
  }
  LARGE_INTEGER ticks;
  FFI(!QueryPerformanceCounter(&ticks) || ticks.QuadPart < 0);
  // Split the conversion so a large uptime cannot overflow the multiply.
  u64 whole = (u64)(ticks.QuadPart / frequency);
  u64 remainder = (u64)(ticks.QuadPart % frequency);
  return whole * 1000000ull + remainder * 1000000ull / (u64)frequency;
}

void Atomic_StoreI32(volatile i32* target, i32 value) {
  InterlockedExchange((volatile LONG*)target, (LONG)value);
}

i32 Atomic_LoadI32(const volatile i32* target) {
  // No plain interlocked load exists; OR with zero reads without mutating.
  return (i32)InterlockedOr((volatile LONG*)target, 0);
}
