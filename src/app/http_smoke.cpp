#include <stdio.h>

#include "base/arena.h"
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

int main(int argc, char** argv) {
  Http_GlobalInit();
  Arena a = ArenaCreate();
  const char* url = argc > 1 ? argv[1] : "https://example.com";

  HttpRequest req = {};
  req.Method = SV("GET");
  req.Url = SV(url);

  Counter c = {};
  HttpStreamSink sink = {};
  sink.User = &c;
  sink.OnData = OnData;

  HttpResult r = Http_Stream(&a, &req, sink, nullptr);
  printf("status=%d bytes=%llu err=%.*s\n", r.Status, (unsigned long long)c.Bytes, (int)r.Error.Length,
         r.Error.Data ? r.Error.Data : "");

  ArenaRelease(&a);
  Http_GlobalShutdown();
  return (r.Status >= 200 && r.Status < 400) ? 0 : 1;
}
