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

bool Thread_Start(Thread* t, ThreadFn fn, void* arg) {
  t->Fn = fn;
  t->Arg = arg;
  HANDLE* h = (HANDLE*)t->Storage;
  *h = CreateThread(nullptr, 0, Trampoline, t, 0, nullptr);
  return *h != nullptr;
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
