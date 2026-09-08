#pragma once

// Path to the test executable itself. Suites that need to re-exec the harness
// as a child process (e.g. the cross-process file lock test) use this.
extern const char* g_test_executable;

int RunAgentBenchmark(int argc, char** argv);

#define CLANKER_TEST_SUITE(name, fn, tag, description) void fn();
#include "test_suites.inc"
#undef CLANKER_TEST_SUITE
