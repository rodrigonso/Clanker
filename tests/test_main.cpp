#include <stdio.h>
#include <string.h>

#include "base/arena.h"
#include "base/process_file_lock.h"
#include "tests.h"

int g_checks = 0;
int g_failures = 0;
const char* g_test_executable = nullptr;

int main(int argc, char** argv) {
  if (argc == 3 && strcmp(argv[1], "--expect-lock-busy") == 0) {
    Arena arena = ArenaCreate();
    ProcessFileLock lock = {};
    ProcessFileLockStatus status =
        ProcessFileLock_TryAcquire(&arena, SV(argv[2]), &lock);
    if (status == ProcessFileLockStatus::Acquired) ProcessFileLock_Release(&lock);
    ArenaRelease(&arena);
    return status == ProcessFileLockStatus::Busy ? 0 : 1;
  }
  g_test_executable = argv[0];
  TestArena();
  TestArray();
  TestStr();
  TestHostInfo();
  TestTypes();
  TestJson();
  TestSession();
  TestHarness();
  TestAnthropic();
  TestTools();
  TestInfra();
  TestCrypto();
  TestAuth();
  TestCommand();
  TestLoopback();
  TestProviderRegistry();
  TestSettings();
  TestThread();
  TestKeys();
  TestScreen();
  TestMarkdown();
  TestSanitize();
  TestStreamReveal();
  TestLineModel();
  TestLineEdit();
  TestTui();
  printf("tests: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
