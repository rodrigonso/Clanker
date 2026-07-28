// Network-tagged suite: exercises the real TLS/HTTP stack against a live
// endpoint. Excluded from the default run; select it explicitly by name or
// with --all. Replaces the old standalone clank_http_smoke executable.
#include <stdio.h>
#include <stdlib.h>

#include "base/arena.h"
#include "check.h"
#include "net/http.h"

namespace {
struct Counter {
  u64 Bytes;
};

bool OnData(void* user, StrView chunk) {
  ((Counter*)user)->Bytes += chunk.Length;
  return true;
}
}  // namespace

void TestHttp() {
  // Overridable so the suite can be pointed at a local server in sandboxed or
  // offline environments.
  const char* url = getenv("CLANK_TEST_HTTP_URL");
  if (!url) url = "https://example.com";

  Http_GlobalInit();
  Arena a = ArenaCreate();

  HttpRequest req = {};
  req.Method = SV("GET");
  req.Url = SV(url);

  Counter c = {};
  HttpStreamSink sink = {};
  sink.User = &c;
  sink.OnData = OnData;

  HttpResult streamed = Http_Stream(&a, &req, sink, nullptr);
  if (streamed.Status < 200 || streamed.Status >= 400) {
    printf("  http: GET %s failed status=%d err=%.*s\n", url, streamed.Status,
           (int)streamed.Error.Length,
           streamed.Error.Data ? streamed.Error.Data : "");
  }
  CHECK_ALL(streamed.Status >= 200, streamed.Status < 400, c.Bytes > 0);

  // The non-streaming path shares the same curl backend; check it agrees.
  HttpResponse fetched = Http_Fetch(&a, &req, nullptr);
  CHECK_ALL(fetched.Status >= 200, fetched.Status < 400,
            fetched.Body.Length > 0);

  ArenaRelease(&a);
  Http_GlobalShutdown();
}
