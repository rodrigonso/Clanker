#include "app/plain_select.h"

#include <stdio.h>

namespace {

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

  char line[64];
  for (;;) {
    fputs("> ", stdout);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) return false;
    u32 choice = 0;
    if (!ParseChoice(line, &choice)) {
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

bool Plain_ReadText(StrView prompt, char* buffer, u32 capacity, StrView* out) {
  if (!buffer || capacity < 2) return false;
  for (;;) {
    printf("%.*s: ", (int)prompt.Length, prompt.Data);
    fflush(stdout);
    if (!fgets(buffer, (int)capacity, stdin)) return false;
    StrView value = TrimLine(buffer);
    if (value.Length == 0) {
      fputs("A value is required.\n", stdout);
      continue;
    }
    if (out) *out = value;
    return true;
  }
}
