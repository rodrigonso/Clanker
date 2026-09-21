#include "app/plain_select.h"

#include <stdio.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

PlainLineResult Plain_ReadLine(char* buffer, u32 capacity) {
  if (!buffer || capacity < 2 || capacity > 0x7fffffff)
    return PlainLineResult::Failed;
  if (!fgets(buffer, (int)capacity, stdin)) {
    Memory_Zero(buffer, capacity);
    return ferror(stdin) ? PlainLineResult::Failed : PlainLineResult::End;
  }
  u32 length = (u32)strlen(buffer);
  if (length && buffer[length - 1] == '\n') return PlainLineResult::Read;
  int next = getchar();
  if (next == '\n' || (next == EOF && !ferror(stdin)))
    return PlainLineResult::Read;
  while (next != '\n' && next != EOF) next = getchar();
  Memory_Zero(buffer, capacity);
  return ferror(stdin) ? PlainLineResult::Failed : PlainLineResult::TooLong;
}

namespace {

struct EchoState {
#if defined(_WIN32)
  HANDLE Input;
  DWORD Mode;
#else
  int Input;
  termios Mode;
#endif
};

bool HideEcho(EchoState* state) {
#if defined(_WIN32)
  intptr_t handle = _get_osfhandle(_fileno(stdin));
  if (handle == -1) return false;
  state->Input = (HANDLE)handle;
  if (!GetConsoleMode(state->Input, &state->Mode)) return false;
  return SetConsoleMode(state->Input, state->Mode & ~ENABLE_ECHO_INPUT) != 0;
#else
  state->Input = fileno(stdin);
  if (!isatty(state->Input) || tcgetattr(state->Input, &state->Mode) != 0) return false;
  termios mode = state->Mode;
  mode.c_lflag &= ~(ECHO | ECHONL);
  return tcsetattr(state->Input, TCSAFLUSH, &mode) == 0;
#endif
}

bool RestoreEcho(const EchoState* state) {
#if defined(_WIN32)
  return SetConsoleMode(state->Input, state->Mode) != 0;
#else
  return tcsetattr(state->Input, TCSANOW, &state->Mode) == 0;
#endif
}

bool ParseChoice(const char* text, u32* value) {
  u32 i = 0;
  while (text[i] == ' ' || text[i] == '\t') i++;
  if (text[i] < '0' || text[i] > '9') return false;
  u32 v = 0;
  while (text[i] >= '0' && text[i] <= '9') {
    u32 digit = (u32)(text[i] - '0');
    if (v > 429496729u || (v == 429496729u && digit > 5)) return false;
    v = v * 10 + digit;
    i++;
  }
  while (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n') i++;
  if (text[i]) return false;
  *value = v;
  return true;
}

StrView TrimLine(char* text) {
  u32 start = 0;
  while (text[start] == ' ' || text[start] == '\t') start++;
  u32 end = start;
  while (text[end]) end++;
  while (end > start &&
         (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
          text[end - 1] == '\n'))
    end--;
  return StrView{text + start, end - start};
}

}  // namespace

bool Plain_Select(StrView title, const StrView* options, u32 optionCount, u32* selected) {
  if (!options || optionCount == 0) return false;
  printf("\n%.*s\n", (int)title.Length, title.Data);
  for (u32 i = 0; i < optionCount; i++)
    printf("  %u. %.*s\n", i + 1, (int)options[i].Length, options[i].Data);
  printf("  0. Cancel\n");

  char line[64] = {};
  for (;;) {
    fputs("> ", stdout);
    fflush(stdout);
    PlainLineResult read = Plain_ReadLine(line, (u32)sizeof(line));
    if (read == PlainLineResult::End || read == PlainLineResult::Failed) {
      Memory_Zero(line, sizeof(line));
      return false;
    }
    u32 choice = 0;
    bool parsed = read == PlainLineResult::Read && ParseChoice(line, &choice);
    Memory_Zero(line, sizeof(line));
    if (!parsed) {
      fputs("Enter a listed number.\n", stdout);
      continue;
    }
    if (choice == 0) return false;
    if (choice <= optionCount) {
      if (selected) *selected = choice - 1;
      return true;
    }
    fputs("Enter a listed number.\n", stdout);
  }
}

bool Plain_ReadText(StrView prompt, char* buffer, u32 capacity, StrView* out,
                    TextInputMode mode, bool* failed) {
  if (out) *out = {};
  if (failed) *failed = false;
  if (!buffer || capacity < 2) {
    if (failed) *failed = true;
    return false;
  }
  bool secret = mode == TextInputMode::Secret;
  EchoState echo = {};
  if (secret && !HideEcho(&echo)) {
    Memory_Zero(buffer, capacity);
    if (failed) *failed = true;
    fputs("Secret input requires an interactive terminal with echo disabled; no input was read.\n",
          stderr);
    return false;
  }
  bool confirmed = false;
  for (;;) {
    printf("%.*s: ", (int)prompt.Length, prompt.Data);
    fflush(stdout);
    PlainLineResult read = Plain_ReadLine(buffer, capacity);
    if (secret) fputc('\n', stdout);
    if (read != PlainLineResult::Read) {
      if (read == PlainLineResult::TooLong || read == PlainLineResult::Failed) {
        if (failed) *failed = true;
        fputs(read == PlainLineResult::TooLong ? "Input is too long; nothing was saved.\n"
                                        : "Input could not be read; nothing was saved.\n",
              stderr);
      }
      break;
    }
    StrView value = TrimLine(buffer);
    if (secret && (!value.Length || memchr(value.Data, '\x03', value.Length))) break;
    if (value.Length == 0) {
      Memory_Zero(buffer, capacity);
      fputs("A value is required.\n", stdout);
      continue;
    }
    if (out) *out = value;
    confirmed = true;
    break;
  }
  if (secret && !RestoreEcho(&echo)) {
    if (failed) *failed = true;
    fputs("Terminal echo could not be restored; nothing was saved.\n", stderr);
    confirmed = false;
  }
  if (!confirmed) {
    if (out) *out = {};
    Memory_Zero(buffer, capacity);
  }
  return confirmed;
}
