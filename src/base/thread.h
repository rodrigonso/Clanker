#pragma once
#include "base/basics.h"

// Minimal threading primitives over Win32 / pthreads. No C++ std. The platform
// objects live in fixed inline storage so this header pulls in no OS headers.

typedef void (*ThreadFn)(void* arg);

struct ThreadPoolTask {
  ThreadFn Fn;
  void* Arg;
};

struct Thread {
  alignas(16) unsigned char Storage[16];  // HANDLE / pthread_t
  ThreadFn Fn;
  void* Arg;
};

bool Thread_Start(Thread* t, ThreadFn fn, void* arg);
void Thread_Join(Thread* t);

// Dispatch short-lived work through the platform's worker facility.
// `task` must remain alive until the submitted callback has completed.
bool ThreadPool_Submit(ThreadPoolTask* task, ThreadFn fn, void* arg);

struct Mutex {
  alignas(16) unsigned char Storage[64];  // CRITICAL_SECTION / pthread_mutex_t
};

void Mutex_Init(Mutex* m);
void Mutex_Destroy(Mutex* m);
void Mutex_Lock(Mutex* m);
void Mutex_Unlock(Mutex* m);

struct CondVar {
  alignas(16) unsigned char Storage[64];  // CONDITION_VARIABLE / pthread_cond_t
};

void Cond_Init(CondVar* c);
void Cond_Destroy(CondVar* c);
void Cond_Wait(CondVar* c, Mutex* m);
void Cond_WaitTimeout(CondVar* c, Mutex* m, u32 ms);  // wake at most after `ms`
void Cond_Signal(CondVar* c);
void Cond_Broadcast(CondVar* c);

// Block the calling thread for at least `ms` milliseconds.
void Sleep_Ms(u32 ms);

// Cancellation flags are set by one thread (a signal handler, a UI action, a
// watchdog) and polled by another. `volatile` keeps the compiler from caching
// the poll but says nothing about the store, and MSVC only gives volatile
// acquire/release semantics under `/volatile:ms`, which this build does not
// pin. Routing both sides through these keeps the flags correct regardless of
// compiler mode.
//
// Relaxed semantics are sufficient and intentional: a cancellation flag
// publishes no data alongside itself, so readers only ever branch on it.
void Atomic_StoreI32(volatile i32* target, i32 value);
i32 Atomic_LoadI32(const volatile i32* target);

// Monotonic milliseconds for measuring short intervals.
u64 MonotonicMillis();

// RAII-free scoped lock helper (no std). Use as: { ScopedLock _l(&m); ... }
struct ScopedLock {
  Mutex* M;
  explicit ScopedLock(Mutex* m) : M(m) { Mutex_Lock(m); }
  ~ScopedLock() { Mutex_Unlock(M); }
  ScopedLock(const ScopedLock&) = delete;
  ScopedLock& operator=(const ScopedLock&) = delete;
};
