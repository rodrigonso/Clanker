#pragma once
#include "base/arena.h"
#include "base/str.h"

// A tiny single-use loopback HTTP server used to capture an OAuth redirect.
// Provider-agnostic; always binds 127.0.0.1 only. No std, caboose conventions.

struct LoopbackServer {
  i64 Sock;  // listening socket handle, -1 when closed
  u16 Port;  // actual bound port (meaningful after Loopback_Open)
};

// Result of waiting for the OAuth redirect.
struct LoopbackCapture {
  bool Ok;        // true if a valid code+state arrived (and state matched)
  StrView Code;   // url-decoded authorization code (arena-owned)
  StrView State;  // url-decoded state (arena-owned)
  StrView Error;  // arena-owned message when !Ok
};

// Bind 127.0.0.1:port and listen. port==0 picks an ephemeral port (read it back
// with Loopback_Port). Returns false and writes `err` on failure.
bool Loopback_Open(LoopbackServer* s, u16 port, StrBuf* err);

// The actual bound port (useful when opened with port==0).
u16 Loopback_Port(const LoopbackServer* s);

// Block until a request hits `path` carrying code+state (validating
// state == expectedState), the user aborts (*abort becomes non-zero), or a
// hard error occurs (provider `error=` or state mismatch). Serves a minimal
// success/error HTML page to the browser. Stray requests (wrong path, missing
// params) are answered and ignored so the wait continues. `abort` may be null.
LoopbackCapture Loopback_WaitForCode(Arena* a, LoopbackServer* s, StrView path, StrView expectedState,
                                     volatile i32* abort);

void Loopback_Close(LoopbackServer* s);

// ---- pure helpers exposed for testing ------------------------------------

// Extract the request-target from a raw HTTP request ("GET <target> HTTP/1.1").
// Splits into `path` (before '?') and `query` (after '?'). Returns false if no
// request line is present.
bool Loopback_ParseRequestLine(StrView raw, StrView* path, StrView* query);

// Find the value of `name` in a urlencoded `query` ("a=1&b=2"). Returns a view
// into `query` (still percent-encoded). {nullptr,0} when absent; {ptr,0} when
// the key is present with an empty value.
StrView Loopback_QueryParam(StrView query, const char* name);

// Percent-decode `s` into the arena ('+' becomes space). Always null-terminated.
StrView Loopback_UrlDecode(Arena* a, StrView s);
