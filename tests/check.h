#pragma once
#include <stdio.h>

extern int g_checks;
extern int g_failures;

template <typename... Conditions>
bool CheckAll(Conditions... conditions) {
  return (... && conditions);
}

#define CHECK(cond)                                            \
  do {                                                         \
    g_checks++;                                                \
    if (!(cond)) {                                             \
      g_failures++;                                            \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
    }                                                          \
  } while (0)

#define CHECK_ALL(...) CHECK(CheckAll(__VA_ARGS__))
