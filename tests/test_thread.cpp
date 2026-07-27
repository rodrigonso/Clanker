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

}  // namespace

void TestThread() {
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
}
