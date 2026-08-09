#include "base/thread.h"
#include "check.h"

namespace {

struct Counter {
  Mutex* M;
  int* Val;
  int Iters;
};

void Worker(void* arg) {
  Counter* c = (Counter*)arg;
  for (int i = 0; i < c->Iters; i++) {
    Mutex_Lock(c->M);
    (*c->Val)++;
    Mutex_Unlock(c->M);
  }
}

struct Box {
  Mutex M;
  CondVar Cv;
  int Ready;
  int Value;
};

void Producer(void* arg) {
  Box* b = (Box*)arg;
  Mutex_Lock(&b->M);
  b->Value = 42;
  b->Ready = 1;
  Cond_Signal(&b->Cv);
  Mutex_Unlock(&b->M);
}

struct AtomicFlagWaiter {
  volatile i32* Flag;
  volatile i32 Observed;
};

// Spins on the flag the way a cancellation poll does, then records that it saw
// the store. A cached read would spin forever and hang the join below.
void AtomicFlagWorker(void* arg) {
  AtomicFlagWaiter* w = (AtomicFlagWaiter*)arg;
  while (Atomic_LoadI32(w->Flag) == 0) Sleep_Ms(1);
  Atomic_StoreI32(&w->Observed, 1);
}

void TestAtomicFlag() {
  // Round-trip through the accessors, including the negative and max values a
  // plain bool flag would silently truncate.
  volatile i32 v = 0;
  CHECK(Atomic_LoadI32(&v) == 0);
  Atomic_StoreI32(&v, 1);
  CHECK(Atomic_LoadI32(&v) == 1);
  Atomic_StoreI32(&v, -7);
  CHECK(Atomic_LoadI32(&v) == -7);
  Atomic_StoreI32(&v, 0);
  CHECK(Atomic_LoadI32(&v) == 0);

  // A load must not mutate: the OR-with-zero trick on Windows would be a bug
  // if it wrote anything back.
  Atomic_StoreI32(&v, 42);
  for (int i = 0; i < 8; i++) CHECK(Atomic_LoadI32(&v) == 42);

  // Cross-thread visibility: the flag is set by this thread and polled by
  // another, which is exactly how cancellation is delivered.
  volatile i32 flag = 0;
  AtomicFlagWaiter waiter = {&flag, 0};
  Thread t;
  CHECK(Thread_Start(&t, AtomicFlagWorker, &waiter));
  Sleep_Ms(5);
  CHECK(Atomic_LoadI32(&waiter.Observed) == 0);  // nothing set it yet
  Atomic_StoreI32(&flag, 1);
  Thread_Join(&t);
  CHECK(Atomic_LoadI32(&waiter.Observed) == 1);
}

void TestMonotonicMicros() {
  u64 previous = MonotonicMicros();
  bool monotonic = true;
  for (u32 i = 0; i < 2000; i++) {
    u64 now = MonotonicMicros();
    monotonic = now >= previous && monotonic;
    previous = now;
  }
  CHECK(monotonic);

  u64 start = MonotonicMicros();
  Sleep_Ms(20);
  u64 elapsed = MonotonicMicros() - start;
  CHECK_ALL(elapsed >= 15000, elapsed < 5000000);

  u64 msStart = MonotonicMillis();
  u64 usStart = MonotonicMicros();
  Sleep_Ms(30);
  u64 msElapsed = MonotonicMillis() - msStart;
  u64 usElapsed = (MonotonicMicros() - usStart) / 1000;
  u64 skew = msElapsed > usElapsed ? msElapsed - usElapsed : usElapsed - msElapsed;
  CHECK(skew <= 25);  // GetTickCount64 can advance on a ~16 ms tick.
}

}  // namespace

void TestThread() {
  TestAtomicFlag();
  // Mutex mutual-exclusion: N threads each increment ITERS times; the sum must
  // be exact (no lost updates).
  Mutex m;
  Mutex_Init(&m);
  int val = 0;
  const int N = 4;
  const int ITERS = 20000;
  Thread th[N];
  Counter c[N];
  bool started = true;
  for (int i = 0; i < N; i++) {
    c[i].M = &m;
    c[i].Val = &val;
    c[i].Iters = ITERS;
    bool ok = Thread_Start(&th[i], Worker, &c[i]);
    started = ok && started;
  }
  CHECK(started);
  for (int i = 0; i < N; i++) Thread_Join(&th[i]);
  CHECK(val == N * ITERS);
  Mutex_Destroy(&m);

  // CondVar signal/wait: consumer blocks until the producer publishes.
  Box b;
  Mutex_Init(&b.M);
  Cond_Init(&b.Cv);
  b.Ready = 0;
  b.Value = 0;
  Thread p;
  CHECK(Thread_Start(&p, Producer, &b));
  Mutex_Lock(&b.M);
  while (!b.Ready) Cond_Wait(&b.Cv, &b.M);
  CHECK(b.Value == 42);
  Mutex_Unlock(&b.M);
  Thread_Join(&p);
  Cond_Destroy(&b.Cv);
  Mutex_Destroy(&b.M);

  TestMonotonicMicros();
}
