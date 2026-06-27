#include <stdio.h>

#include "tests.h"

int g_checks = 0;
int g_failures = 0;

int main() {
  TestArena();
  TestArray();
  TestStr();
  TestTypes();
  TestJson();
  TestAgent();
  TestAnthropic();
  TestTools();
  TestCrypto();
  TestAuth();
  TestLoopback();
  printf("tests: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
