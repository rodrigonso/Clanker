#pragma once
#include "base/str.h"

// Numbered selection and bounded manual input for the non-TUI REPL.
bool Plain_Select(StrView title, const StrView* options, u32 optionCount, u32* selected);
bool Plain_ReadText(StrView prompt, char* buffer, u32 capacity, StrView* out);
