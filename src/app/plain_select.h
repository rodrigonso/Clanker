#pragma once
#include "base/str.h"
#include "ui/lineedit.h"

enum class PlainLineResult : u8 { Read, End, TooLong, Failed };

// Reads one bounded stdin line; overlong lines are fully drained and rejected.
PlainLineResult Plain_ReadLine(char* buffer, u32 capacity);

// Numbered selection and bounded manual input for the non-TUI REPL.
bool Plain_Select(StrView title, const StrView* options, u32 optionCount, u32* selected);
bool Plain_ReadText(StrView prompt, char* buffer, u32 capacity, StrView* out,
                    TextInputMode mode = TextInputMode::Plain,
                    bool* failed = nullptr);
