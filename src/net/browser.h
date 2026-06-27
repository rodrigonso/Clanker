#pragma once
#include "base/str.h"

// Open `url` in the user's default browser (best-effort, fire-and-forget).
// Windows: ShellExecuteW. macOS: `open`. Linux/other: `xdg-open`.
void OpenBrowser(StrView url);
