#pragma once
#include "base/str.h"

// Sanitize external (tool) output before it enters the display transcript.
//
// Tool output (e.g. a Windows `cmd.exe` command run via the bash tool) can carry
// carriage returns (CRLF line endings), tabs, and ANSI/VT escape sequences. The
// differential renderer stores one cell per scalar and re-emits it verbatim, so
// a raw `\r` would snap the real cursor back to column 0 mid-row -- desyncing it
// from the renderer's model and leaving stale cells on the right un-erased -- and
// an embedded escape sequence or tab would likewise move the cursor out from
// under the cell grid. This pass removes those at the boundary:
//
//   - ANSI/VT escape sequences (CSI `ESC [ ... `, OSC `ESC ] ... `, and other
//     `ESC`-introduced forms) are dropped.
//   - Carriage returns and other C0 control bytes (and DEL 0x7F) are dropped.
//   - Tabs are expanded to spaces (to the next 8-column tab stop, per line).
//   - Newlines are preserved (the line model splits the transcript on them).
//
// Cleaned UTF-8 is appended to `out`. Mirrors pi's getTextOutput() sanitize
// chain (stripAnsi -> drop control chars -> strip carriage returns).
void Sanitize_Display(StrBuf* out, StrView in);
