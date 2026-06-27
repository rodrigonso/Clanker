#include "net/loopback.h"

#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int ma_socklen_t;
#else
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
typedef socklen_t ma_socklen_t;
#define INVALID_SOCKET (-1)
#endif

namespace {

#if defined(_WIN32)
bool g_wsaUp = false;
bool NetInit() {
  if (g_wsaUp) return true;
  WSADATA d;
  if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return false;
  g_wsaUp = true;
  return true;
}
void CloseSock(SOCKET fd) { closesocket(fd); }
#else
bool NetInit() { return true; }
void CloseSock(SOCKET fd) { close(fd); }
#endif

void SendAll(SOCKET c, const char* data, u32 len) {
  u32 off = 0;
  while (off < len) {
    int n = (int)send(c, data + off, (int)(len - off), 0);
    if (n <= 0) break;
    off += (u32)n;
  }
}

StrView SuccessHtml() {
  return SVL(
      "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
      "<title>Login complete</title><style>body{font-family:system-ui,-apple-system,sans-serif;"
      "background:#0b0b0c;color:#fafafa;display:grid;place-items:center;height:100vh;margin:0}"
      ".card{text-align:center;padding:2rem 3rem;border:1px solid #2a2a2e;border-radius:12px}"
      "h1{font-size:1.25rem;margin:0 0 .5rem}p{color:#a1a1aa;margin:0}</style></head><body>"
      "<div class=\"card\"><h1>&#10003; Authentication complete</h1>"
      "<p>You can close this window and return to your terminal.</p></div></body></html>");
}

StrView BuildErrorHtml(Arena* a, StrView message) {
  StrBuf b = StrBuf_Make(a, message.Length + 512);
  StrBuf_AppendCStr(
      &b,
      "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
      "<title>Login error</title><style>body{font-family:system-ui,-apple-system,sans-serif;"
      "background:#0b0b0c;color:#fafafa;display:grid;place-items:center;height:100vh;margin:0}"
      ".card{text-align:center;padding:2rem 3rem;border:1px solid #3a1d1d;border-radius:12px}"
      "h1{font-size:1.25rem;margin:0 0 .5rem;color:#f87171}p{color:#a1a1aa;margin:0}</style></head>"
      "<body><div class=\"card\"><h1>Authentication error</h1><p>");
  for (u32 i = 0; i < message.Length; i++) {
    char c = message.Data[i];
    if (c == '<')
      StrBuf_AppendCStr(&b, "&lt;");
    else if (c == '>')
      StrBuf_AppendCStr(&b, "&gt;");
    else if (c == '&')
      StrBuf_AppendCStr(&b, "&amp;");
    else
      StrBuf_AppendChar(&b, c);
  }
  StrBuf_AppendCStr(&b, "</p></div></body></html>");
  return StrBuf_View(&b);
}

void SendResponse(Arena* a, SOCKET c, int status, const char* statusText, StrView html) {
  StrBuf b = StrBuf_Make(a, html.Length + 160);
  StrBuf_AppendCStr(&b, "HTTP/1.1 ");
  StrBuf_AppendI64(&b, status);
  StrBuf_AppendChar(&b, ' ');
  StrBuf_AppendCStr(&b, statusText);
  StrBuf_AppendCStr(&b,
                    "\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: ");
  StrBuf_AppendU64(&b, html.Length);
  StrBuf_AppendCStr(&b, "\r\n\r\n");
  StrBuf_Append(&b, html);
  SendAll(c, b.Data, b.Length);
}

int HexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool Loopback_ParseRequestLine(StrView raw, StrView* path, StrView* query) {
  u32 i = 0;
  while (i < raw.Length && raw.Data[i] != ' ') i++;  // skip method
  if (i >= raw.Length) return false;
  i++;  // skip the space
  u32 start = i;
  while (i < raw.Length && raw.Data[i] != ' ' && raw.Data[i] != '\r' && raw.Data[i] != '\n') i++;
  StrView target{raw.Data + start, i - start};
  if (target.Length == 0) return false;
  u32 q = 0;
  while (q < target.Length && target.Data[q] != '?') q++;
  *path = StrView{target.Data, q};
  if (q < target.Length)
    *query = StrView{target.Data + q + 1, target.Length - q - 1};
  else
    *query = StrView{nullptr, 0};
  return true;
}

StrView Loopback_QueryParam(StrView q, const char* name) {
  u32 nl = (u32)strlen(name);
  u32 i = 0;
  while (i < q.Length) {
    u32 start = i;
    while (i < q.Length && q.Data[i] != '&') i++;
    StrView pair{q.Data + start, i - start};
    u32 e = 0;
    while (e < pair.Length && pair.Data[e] != '=') e++;
    if (e == nl && memcmp(pair.Data, name, nl) == 0) {
      if (e < pair.Length) return StrView{pair.Data + e + 1, pair.Length - e - 1};
      return StrView{pair.Data + e, 0};  // present, empty value
    }
    if (i < q.Length) i++;  // skip '&'
  }
  return StrView{nullptr, 0};
}

StrView Loopback_UrlDecode(Arena* a, StrView s) {
  char* out = (char*)ArenaAlloc(a, s.Length + 1, 1);
  u32 n = 0;
  for (u32 i = 0; i < s.Length; i++) {
    char c = s.Data[i];
    if (c == '+') {
      out[n++] = ' ';
    } else if (c == '%' && i + 2 < s.Length) {
      int hi = HexVal(s.Data[i + 1]);
      int lo = HexVal(s.Data[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out[n++] = (char)((hi << 4) | lo);
        i += 2;
      } else {
        out[n++] = c;
      }
    } else {
      out[n++] = c;
    }
  }
  out[n] = '\0';
  return StrView{out, n};
}

bool Loopback_Open(LoopbackServer* s, u16 port, StrBuf* err) {
  s->Sock = -1;
  s->Port = 0;
  if (!NetInit()) {
    StrBuf_AppendCStr(err, "network init failed");
    return false;
  }
  SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd == INVALID_SOCKET) {
    StrBuf_AppendCStr(err, "socket() failed");
    return false;
  }
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1
  addr.sin_port = htons(port);
  if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
    StrBuf_AppendCStr(err, "bind 127.0.0.1:");
    StrBuf_AppendU64(err, port);
    StrBuf_AppendCStr(err, " failed (port already in use?)");
    CloseSock(fd);
    return false;
  }
  if (listen(fd, 8) != 0) {
    StrBuf_AppendCStr(err, "listen() failed");
    CloseSock(fd);
    return false;
  }
  sockaddr_in bound;
  memset(&bound, 0, sizeof(bound));
  ma_socklen_t bl = sizeof(bound);
  if (getsockname(fd, (sockaddr*)&bound, &bl) == 0)
    s->Port = ntohs(bound.sin_port);
  else
    s->Port = port;
  s->Sock = (i64)fd;
  return true;
}

u16 Loopback_Port(const LoopbackServer* s) { return s->Port; }

void Loopback_Close(LoopbackServer* s) {
  if (s->Sock >= 0) {
    CloseSock((SOCKET)s->Sock);
    s->Sock = -1;
  }
}

LoopbackCapture Loopback_WaitForCode(Arena* a, LoopbackServer* s, StrView path, StrView expectedState,
                                     volatile i32* abort) {
  LoopbackCapture cap;
  cap.Ok = false;
  cap.Code = StrView{nullptr, 0};
  cap.State = StrView{nullptr, 0};
  cap.Error = StrView{nullptr, 0};

  SOCKET listenFd = (SOCKET)s->Sock;
  for (;;) {
    if (abort && *abort) {
      cap.Error = SVL("login cancelled");
      return cap;
    }
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listenFd, &rfds);
    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 250000;  // 250ms, so Ctrl+C is honored promptly
    int sel = select((int)listenFd + 1, &rfds, nullptr, nullptr, &tv);
    if (sel <= 0) continue;  // timeout or EINTR -> re-check abort

    SOCKET c = accept(listenFd, nullptr, nullptr);
    if (c == INVALID_SOCKET) continue;

    usize mark = a->Used;  // roll back per-request scratch on the stray-request paths

    char buf[8192];
    u32 total = 0;
    while (total < sizeof(buf) - 1) {
      int n = (int)recv(c, buf + total, (int)(sizeof(buf) - 1 - total), 0);
      if (n <= 0) break;
      total += (u32)n;
      bool sawEol = false;
      for (u32 k = 0; k < total; k++)
        if (buf[k] == '\n') {
          sawEol = true;
          break;
        }
      if (sawEol) break;
    }
    buf[total] = '\0';

    StrView reqPath, query;
    if (!Loopback_ParseRequestLine(StrView{buf, total}, &reqPath, &query)) {
      SendResponse(a, c, 400, "Bad Request", BuildErrorHtml(a, SVL("Malformed request.")));
      CloseSock(c);
      ArenaRestore(a, mark);
      continue;
    }
    if (!StrView_Eq(reqPath, path)) {
      SendResponse(a, c, 404, "Not Found", BuildErrorHtml(a, SVL("Callback route not found.")));
      CloseSock(c);
      ArenaRestore(a, mark);
      continue;  // stray request (e.g. favicon) -> keep waiting
    }

    StrView errParam = Loopback_QueryParam(query, "error");
    if (errParam.Data) {
      StrView msg = Loopback_UrlDecode(a, errParam);
      StrBuf eb = StrBuf_Make(a, msg.Length + 48);
      StrBuf_AppendCStr(&eb, "authorization denied: ");
      StrBuf_Append(&eb, msg);
      SendResponse(a, c, 400, "Bad Request",
                   BuildErrorHtml(a, SVL("Anthropic authentication did not complete.")));
      CloseSock(c);
      cap.Error = StrBuf_View(&eb);
      return cap;
    }

    StrView code = Loopback_QueryParam(query, "code");
    StrView state = Loopback_QueryParam(query, "state");
    if (!code.Data || !state.Data) {
      SendResponse(a, c, 400, "Bad Request",
                   BuildErrorHtml(a, SVL("Missing code or state parameter.")));
      CloseSock(c);
      ArenaRestore(a, mark);
      continue;  // probe / partial request -> keep waiting
    }

    StrView codeD = Loopback_UrlDecode(a, code);
    StrView stateD = Loopback_UrlDecode(a, state);
    if (expectedState.Length && !StrView_Eq(stateD, expectedState)) {
      SendResponse(a, c, 400, "Bad Request", BuildErrorHtml(a, SVL("State mismatch.")));
      CloseSock(c);
      cap.Error = SVL("OAuth state mismatch");
      return cap;
    }

    SendResponse(a, c, 200, "OK", SuccessHtml());
    CloseSock(c);
    cap.Ok = true;
    cap.Code = codeD;
    cap.State = stateD;
    return cap;
  }
}
