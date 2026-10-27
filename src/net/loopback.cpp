#include "net/loopback.h"

#include <stdio.h>
#include <string.h>

#include "base/thread.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>

using SockLen = int;
using NativeSocket = SOCKET;
static void CloseSocket(LoopbackSocket s) { closesocket((SOCKET)s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

using SockLen = socklen_t;
using NativeSocket = int;
static void CloseSocket(LoopbackSocket s) { close((int)s); }
#endif

namespace {

const u32 REQUEST_HEAD_CAPACITY = 8192;
const u32 CLIENT_READ_TIMEOUT_MS = 5000;
const u32 POLL_MS = 100;

// Waits until `s` is readable or `ms` elapses. Returns <0 on error.
int WaitReadable(LoopbackSocket s, u32 ms) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET((NativeSocket)s, &set);
  timeval tv = {};
  tv.tv_sec = (long)(ms / 1000);
  tv.tv_usec = (int)((ms % 1000) * 1000);
  return select((int)s + 1, &set, nullptr, nullptr, &tv);
}

void SendAll(LoopbackSocket s, StrView data) {
  u32 sent = 0;
  while (sent < data.Length) {
    int n = (int)send((NativeSocket)s, data.Data + sent, (int)(data.Length - sent), 0);
    if (n <= 0) return;
    sent += (u32)n;
  }
}

StrView ReasonPhrase(u32 status) {
  switch (status) {
    case 200: return SVL("OK");
    case 400: return SVL("Bad Request");
    case 404: return SVL("Not Found");
    case 405: return SVL("Method Not Allowed");
    default: return SVL("Error");
  }
}

void RespondAndClose(LoopbackSocket s, u32 status, StrView html) {
  char head[256];
  StrView reason = ReasonPhrase(status);
  int length = snprintf(head, sizeof(head),
                        "HTTP/1.1 %u %.*s\r\n"
                        "Content-Type: text/html; charset=utf-8\r\n"
                        "Content-Length: %u\r\n"
                        "Cache-Control: no-store\r\n"
                        "Connection: close\r\n\r\n",
                        status, (int)reason.Length, reason.Data, html.Length);
  if (length > 0 && (u32)length < sizeof(head)) {
    SendAll(s, StrView{head, (u32)length});
    SendAll(s, html);
  }
#if defined(_WIN32)
  shutdown((SOCKET)s, SD_SEND);
#else
  shutdown((int)s, SHUT_WR);
#endif
  CloseSocket(s);
}

// Reads the request head (through the blank line) so closing the socket does
// not reset the connection under the browser while it still has bytes queued.
// `buf` holds REQUEST_HEAD_CAPACITY + 1 bytes.
bool ReadHead(LoopbackSocket s, char* buf, StrView* out) {
  u32 length = 0;
  u64 deadline = MonotonicMillis() + CLIENT_READ_TIMEOUT_MS;
  while (length < REQUEST_HEAD_CAPACITY) {
    u64 now = MonotonicMillis();
    if (now >= deadline) return false;
    if (WaitReadable(s, (u32)(deadline - now)) <= 0) return false;
    int n = (int)recv((NativeSocket)s, buf + length, (int)(REQUEST_HEAD_CAPACITY - length), 0);
    if (n <= 0) return false;
    length += (u32)n;
    buf[length] = '\0';
    if (strstr(buf, "\r\n\r\n")) {
      *out = StrView{buf, length};
      return true;
    }
  }
  return false;
}

// "GET <target> HTTP/1.x" -> target.
bool ParseRequestLine(StrView head, bool* isGet, StrView* target) {
  const char* end = (const char*)memchr(head.Data, '\r', head.Length);
  if (!end) return false;
  StrView line = {head.Data, (u32)(end - head.Data)};
  const char* firstSpace = (const char*)memchr(line.Data, ' ', line.Length);
  if (!firstSpace) return false;
  u32 methodLength = (u32)(firstSpace - line.Data);
  StrView rest = {firstSpace + 1, line.Length - methodLength - 1};
  const char* secondSpace = (const char*)memchr(rest.Data, ' ', rest.Length);
  if (!secondSpace) return false;
  StrView version = {secondSpace + 1, rest.Length - (u32)(secondSpace - rest.Data) - 1};
  if (!StrView_StartsWith(version, SVL("HTTP/1."))) return false;
  *isGet = StrView_Eq(StrView{line.Data, methodLength}, SVL("GET"));
  *target = StrView{rest.Data, (u32)(secondSpace - rest.Data)};
  return target->Length > 0;
}

}  // namespace

bool LoopbackServer_Open(LoopbackServer* server) {
  *server = LoopbackServer{};
#if defined(_WIN32)
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    WSACleanup();
    return false;
  }
  // Without this another local process could bind the same port with
  // SO_REUSEADDR and receive the authorization code instead of us.
  BOOL exclusive = TRUE;
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive,
             sizeof(exclusive));
#else
  int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s < 0) return false;
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  SockLen addrLength = (SockLen)sizeof(addr);
  if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(s, 8) != 0 ||
      getsockname(s, (sockaddr*)&addr, &addrLength) != 0) {
    CloseSocket((LoopbackSocket)s);
#if defined(_WIN32)
    WSACleanup();
#endif
    return false;
  }
  server->Socket = (LoopbackSocket)s;
  server->Port = ntohs(addr.sin_port);
  return true;
}

void LoopbackServer_Close(LoopbackServer* server) {
  if (!server || server->Socket == LOOPBACK_INVALID_SOCKET) return;
  CloseSocket(server->Socket);
  server->Socket = LOOPBACK_INVALID_SOCKET;
#if defined(_WIN32)
  WSACleanup();
#endif
}

LoopbackWait LoopbackServer_Accept(LoopbackServer* server, Arena* arena,
                                   volatile i32* abort, u32 timeoutMs,
                                   LoopbackRequest* out) {
  *out = LoopbackRequest{};
  if (!server || server->Socket == LOOPBACK_INVALID_SOCKET) return LoopbackWait::Failed;
  u64 deadline = MonotonicMillis() + timeoutMs;
  for (;;) {
    if (abort && Atomic_LoadI32(abort)) return LoopbackWait::Aborted;
    u64 now = MonotonicMillis();
    if (now >= deadline) return LoopbackWait::TimedOut;
    u64 remaining = deadline - now;
    int ready = WaitReadable(server->Socket, remaining < POLL_MS ? (u32)remaining : POLL_MS);
    if (ready < 0) return LoopbackWait::Failed;
    if (ready == 0) continue;

#if defined(_WIN32)
    SOCKET accepted = accept((SOCKET)server->Socket, nullptr, nullptr);
    if (accepted == INVALID_SOCKET) continue;
#else
    int accepted = accept((int)server->Socket, nullptr, nullptr);
    if (accepted < 0) continue;
    int one = 1;
    setsockopt(accepted, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    LoopbackSocket client = (LoopbackSocket)accepted;
    // Skipped connections must cost no arena memory, or a local process could
    // exhaust the caller's arena by connecting repeatedly.
    char buf[REQUEST_HEAD_CAPACITY + 1];
    StrView head = {};
    bool isGet = false;
    StrView target = {};
    if (!ReadHead(client, buf, &head) || !ParseRequestLine(head, &isGet, &target)) {
      RespondAndClose(client, 400, SVL("Bad request."));
      continue;
    }
    if (!isGet) {
      RespondAndClose(client, 405, SVL("Method not allowed."));
      continue;
    }
    out->Socket = client;
    out->Target = StrView_Dup(arena, target);
    return LoopbackWait::Request;
  }
}

void LoopbackRequest_Respond(LoopbackRequest* request, u32 status, StrView html) {
  if (!request || request->Socket == LOOPBACK_INVALID_SOCKET) return;
  RespondAndClose(request->Socket, status, html);
  request->Socket = LOOPBACK_INVALID_SOCKET;
}
