#pragma once
#include "base/arena.h"
#include "base/str.h"

// A one-shot HTTP listener on 127.0.0.1 for OAuth redirects. It understands
// just enough HTTP to read a GET request line and answer with a small page.

using LoopbackSocket = uintptr_t;
#define LOOPBACK_INVALID_SOCKET (~(LoopbackSocket)0)

struct LoopbackServer {
  LoopbackSocket Socket = LOOPBACK_INVALID_SOCKET;
  u16 Port;
};

struct LoopbackRequest {
  LoopbackSocket Socket = LOOPBACK_INVALID_SOCKET;
  StrView Target;  // the request-target, e.g. "/callback?code=..."
};

enum class LoopbackWait : u8 { Request, Aborted, TimedOut, Failed };

// Binds an OS-assigned port on 127.0.0.1 only.
bool LoopbackServer_Open(LoopbackServer* server);
void LoopbackServer_Close(LoopbackServer* server);

// Waits for the next well-formed GET request. Malformed or non-GET requests are
// answered and skipped. `abort` is polled at least every 100 ms.
LoopbackWait LoopbackServer_Accept(LoopbackServer* server, Arena* arena,
                                   volatile i32* abort, u32 timeoutMs,
                                   LoopbackRequest* out);

// Sends a complete HTML response and closes the connection.
void LoopbackRequest_Respond(LoopbackRequest* request, u32 status, StrView html);
