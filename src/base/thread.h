#pragma once
#include "base/basics.h"

// Minimal threading primitives over Win32 / pthreads. No C++ std. The platform
// objects live in fixed inline storage so this header pulls in no OS headers.

typedef void (*ThreadFn)(void* arg);

struct Thread {
  alignas(16) unsigned char Storage[16];  // HANDLE / pthread_t
  ThreadFn Fn;
  void* Arg;
};

bool Thread_Start(Thread* t, ThreadFn fn, void* arg);
void Thread_Join(Thread* t);

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
