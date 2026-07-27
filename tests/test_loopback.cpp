#include <string.h>

#include "auth/oauth.h"
#include "base/arena.h"
#include "base/str.h"
#include "base/thread.h"
#include "check.h"
#include "net/loopback.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#endif

static SOCKET ConnectLoopback(u16 port) {
  SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd == INVALID_SOCKET) return fd;
  sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(port);
  if (connect(fd, (sockaddr*)&a, sizeof(a)) != 0) {
#if defined(_WIN32)
    closesocket(fd);
#else
    close(fd);
#endif
    return INVALID_SOCKET;
  }
  return fd;
}

static void CloseClient(SOCKET fd) {
#if defined(_WIN32)
  closesocket(fd);
#else
  close(fd);
#endif
}

static void TestParseHelpers() {
  Arena a = ArenaCreate();

  StrView path, query;
  CHECK(Loopback_ParseRequestLine(SVL("GET /callback?code=abc&state=xyz HTTP/1.1\r\n"), &path, &query));
  CHECK(StrView_EqCStr(path, "/callback"));
  CHECK(StrView_EqCStr(query, "code=abc&state=xyz"));
  CHECK(StrView_EqCStr(Loopback_QueryParam(query, "code"), "abc"));
  CHECK(StrView_EqCStr(Loopback_QueryParam(query, "state"), "xyz"));
  CHECK(Loopback_QueryParam(query, "missing").Data == nullptr);

  // A path with no query yields an empty query view.
  StrView p2, q2;
  CHECK(Loopback_ParseRequestLine(SVL("GET /favicon.ico HTTP/1.1\r\n"), &p2, &q2));
  CHECK(StrView_EqCStr(p2, "/favicon.ico"));
  CHECK(q2.Length == 0);

  // error= is extracted like any other param.
  StrView pe, qe;
  CHECK(Loopback_ParseRequestLine(SVL("GET /callback?error=access_denied HTTP/1.1\r\n"), &pe, &qe));
  CHECK(StrView_EqCStr(Loopback_QueryParam(qe, "error"), "access_denied"));

  // Percent + '+' decoding.
  CHECK(StrView_EqCStr(Loopback_UrlDecode(&a, SVL("a%20b%2Bc+d")), "a b+c d"));

  ArenaRelease(&a);
}

static void TestParseRedirect() {
  OAuthRedirect rd = OAuth_ParseRedirect(SVL("http://localhost:53692/callback"));
  CHECK(rd.Port == 53692);
  CHECK(StrView_EqCStr(rd.Path, "/callback"));

  OAuthRedirect rd2 = OAuth_ParseRedirect(SVL("https://example.com/cb"));
  CHECK(rd2.Port == 443);
  CHECK(StrView_EqCStr(rd2.Path, "/cb"));
}

// Drive the real socket path in-process: open the server on an ephemeral port,
// connect a client into the backlog, send a callback request, and confirm the
// server captures code+state and answers 200.
static void TestCaptureRoundTrip() {
  Arena a = ArenaCreate();
  StrBuf err = StrBuf_Make(&a, 64);

  LoopbackServer srv;
  CHECK(Loopback_Open(&srv, 0, &err));  // 0 = ephemeral port
  u16 port = srv.Port;
  CHECK(port != 0);

  SOCKET cs = ConnectLoopback(port);
  CHECK(cs != INVALID_SOCKET);

  const char* req = "GET /callback?code=abc123&state=tok-xyz HTTP/1.1\r\nHost: localhost\r\n\r\n";
  send(cs, req, (int)strlen(req), 0);

  LoopbackCapture cap = Loopback_WaitForCode(&a, &srv, SVL("/callback"), SVL("tok-xyz"), nullptr);
  CHECK(cap.Ok);
  CHECK(StrView_EqCStr(cap.Code, "abc123"));
  CHECK(StrView_EqCStr(cap.State, "tok-xyz"));

  char resp[2048];
  int n = (int)recv(cs, resp, sizeof(resp) - 1, 0);
  CHECK(n > 0);
  if (n > 0) {
    StrView r{resp, (u32)n};
    CHECK(StrView_Contains(r, "200 OK"));
    CHECK(StrView_Contains(r, "Authentication complete"));
  }

  CloseClient(cs);
  Loopback_Close(&srv);
  ArenaRelease(&a);
}

// A mismatched state is rejected with a 400 and a failed capture.
static void TestStateMismatch() {
  Arena a = ArenaCreate();
  StrBuf err = StrBuf_Make(&a, 64);

  LoopbackServer srv;
  CHECK(Loopback_Open(&srv, 0, &err));
  u16 port = srv.Port;

  SOCKET cs = ConnectLoopback(port);
  CHECK(cs != INVALID_SOCKET);

  const char* req = "GET /callback?code=abc&state=WRONG HTTP/1.1\r\n\r\n";
  send(cs, req, (int)strlen(req), 0);

  LoopbackCapture cap = Loopback_WaitForCode(&a, &srv, SVL("/callback"), SVL("expected"), nullptr);
  CHECK(!cap.Ok);
  CHECK(cap.Error.Length > 0);

  char resp[2048];
  int n = (int)recv(cs, resp, sizeof(resp) - 1, 0);
  CHECK(n > 0);
  if (n > 0) {
    StrView r{resp, (u32)n};
    CHECK(StrView_Contains(r, "400"));
  }

  CloseClient(cs);
  Loopback_Close(&srv);
  ArenaRelease(&a);
}

struct CancelWaitContext {
  Arena* Mem;
  LoopbackServer* Server;
  volatile i32* Abort;
  LoopbackCapture Capture;
};

static void RunCancelledWait(void* user) {
  CancelWaitContext* ctx = (CancelWaitContext*)user;
  ctx->Capture =
      Loopback_WaitForCode(ctx->Mem, ctx->Server, SVL("/callback"), SVL("expected"), ctx->Abort);
}

// A client that connects without sending a request must not trap the callback
// server inside a blocking recv; cancellation still completes promptly.
static void TestCancelStalledClient() {
  Arena a = ArenaCreate();
  StrBuf err = StrBuf_Make(&a, 64);
  LoopbackServer srv;
  CHECK(Loopback_Open(&srv, 0, &err));

  SOCKET cs = ConnectLoopback(srv.Port);
  CHECK(cs != INVALID_SOCKET);

  volatile i32 abort = 0;
  CancelWaitContext ctx = {&a, &srv, &abort, LoopbackCapture{}};
  Thread waiter = {};
  CHECK(Thread_Start(&waiter, RunCancelledWait, &ctx));
  Sleep_Ms(100);
  abort = 1;
  Thread_Join(&waiter);

  CHECK(!ctx.Capture.Ok);
  CHECK(StrView_Contains(ctx.Capture.Error, "cancelled"));
  CloseClient(cs);
  Loopback_Close(&srv);
  ArenaRelease(&a);
}

void TestLoopback() {
  TestParseHelpers();
  TestParseRedirect();
  TestCaptureRoundTrip();
  TestStateMismatch();
  TestCancelStalledClient();
}
