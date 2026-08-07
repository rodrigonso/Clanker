#include "base/thread.h"

#include <pthread.h>
#include <time.h>

static_assert(sizeof(pthread_t) <= 16, "Thread storage too small");
static_assert(sizeof(pthread_mutex_t) <= 64, "Mutex storage too small");
static_assert(sizeof(pthread_cond_t) <= 64, "CondVar storage too small");

static void* Trampoline(void* p) {
  Thread* t = (Thread*)p;
  t->Fn(t->Arg);
  return nullptr;
}

static void* PoolTrampoline(void* context) {
  ThreadPoolTask* task = (ThreadPoolTask*)context;
  task->Fn(task->Arg);
  return nullptr;
}

bool Thread_Start(Thread* t, ThreadFn fn, void* arg) {
  t->Fn = fn;
  t->Arg = arg;
  pthread_t* th = (pthread_t*)t->Storage;
  return pthread_create(th, nullptr, Trampoline, t) == 0;
}

bool ThreadPool_Submit(ThreadPoolTask* task, ThreadFn fn, void* arg) {
  task->Fn = fn;
  task->Arg = arg;
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) return false;
  pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  int created = pthread_create(&thread, &attributes, PoolTrampoline, task);
  pthread_attr_destroy(&attributes);
  return created == 0;
}

void Thread_Join(Thread* t) {
  pthread_t* th = (pthread_t*)t->Storage;
  pthread_join(*th, nullptr);
}

void Mutex_Init(Mutex* m) { pthread_mutex_init((pthread_mutex_t*)m->Storage, nullptr); }
void Mutex_Destroy(Mutex* m) { pthread_mutex_destroy((pthread_mutex_t*)m->Storage); }
void Mutex_Lock(Mutex* m) { pthread_mutex_lock((pthread_mutex_t*)m->Storage); }
void Mutex_Unlock(Mutex* m) { pthread_mutex_unlock((pthread_mutex_t*)m->Storage); }

void Cond_Init(CondVar* c) { pthread_cond_init((pthread_cond_t*)c->Storage, nullptr); }
void Cond_Destroy(CondVar* c) { pthread_cond_destroy((pthread_cond_t*)c->Storage); }
void Cond_Wait(CondVar* c, Mutex* m) {
  pthread_cond_wait((pthread_cond_t*)c->Storage, (pthread_mutex_t*)m->Storage);
}
void Cond_WaitTimeout(CondVar* c, Mutex* m, u32 ms) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += (time_t)(ms / 1000);
  ts.tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000L;
  }
  pthread_cond_timedwait((pthread_cond_t*)c->Storage, (pthread_mutex_t*)m->Storage, &ts);
}
void Cond_Signal(CondVar* c) { pthread_cond_signal((pthread_cond_t*)c->Storage); }
void Cond_Broadcast(CondVar* c) { pthread_cond_broadcast((pthread_cond_t*)c->Storage); }

void Sleep_Ms(u32 ms) {
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000);
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, nullptr);
}

u64 MonotonicMillis() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 1000ull + (u64)(ts.tv_nsec / 1000000L);
}

void Atomic_StoreI32(volatile i32* target, i32 value) {
  __atomic_store_n(target, value, __ATOMIC_SEQ_CST);
}

i32 Atomic_LoadI32(const volatile i32* target) {
  return __atomic_load_n(target, __ATOMIC_SEQ_CST);
}
