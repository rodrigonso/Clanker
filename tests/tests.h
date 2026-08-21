#pragma once

// Path to the test executable itself. Suites that need to re-exec the harness
// as a child process (e.g. the cross-process file lock test) use this.
extern const char* g_test_executable;

int RunAgentBenchmark(int argc, char** argv);

void TestArena();
void TestArray();
void TestStr();
void TestHostInfo();
void TestTypes();
void TestJson();
void TestSession();
void TestHarness();
void TestPermissions();
void TestCompaction();
void TestEventJson();
void TestResponses();
void TestTools();
void TestToolSelect();
void TestInfra();
void TestCrypto();
void TestAuth();
void TestCommand();
void TestApp();
void TestHooks();
void TestPromptContext();
void TestPromptTemplates();
void TestProjectTrust();
void TestProviderRegistry();
void TestSettings();
void TestSkills();
void TestThread();
void TestKeys();
void TestScreen();
void TestMarkdown();
void TestSanitize();
void TestStreamReveal();
void TestLineModel();
void TestLineEdit();
void TestUi();
void TestUiWidgets();
void TestUiDemo();
void TestTui();
void TestHttp();
void TestHttpLive();
