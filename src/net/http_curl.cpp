#include <curl/curl.h>

#include "base/str.h"
#include "net/http.h"

namespace {

struct WriteCtx {
  HttpStreamSink sink;
  volatile i32* abort;
  bool aborted;
};

size_t WriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  WriteCtx* w = (WriteCtx*)userdata;
  size_t n = size * nmemb;
  if (w->abort && *w->abort) {
    w->aborted = true;
    return 0;
  }
  StrView chunk{ptr, (u32)n};
  if (w->sink.OnData && !w->sink.OnData(w->sink.User, chunk)) {
    w->aborted = true;
    return 0;
  }
  return n;
}

int XferCb(void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  WriteCtx* w = (WriteCtx*)p;
  if (w->abort && *w->abort) {
    w->aborted = true;
    return 1;  // non-zero aborts the transfer
  }
  return 0;
}

}  // namespace

void Http_GlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
void Http_GlobalShutdown() { curl_global_cleanup(); }

HttpResult Http_Stream(Arena* arena, const HttpRequest* req, HttpStreamSink sink, volatile i32* abort) {
  HttpResult res = {};
  res.Status = -1;

  CURL* curl = curl_easy_init();
  if (!curl) {
    res.Error = SV("curl init failed");
    return res;
  }

  StrBuf url = StrBuf_Make(arena, req->Url.Length + 1);
  StrBuf_Append(&url, req->Url);
  curl_easy_setopt(curl, CURLOPT_URL, StrBuf_CStr(&url));

  if (StrView_EqCStr(req->Method, "POST")) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->Body.Data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req->Body.Length);
  }

  struct curl_slist* hdrs = nullptr;
  for (u32 i = 0; i < req->HeaderCount; i++) {
    StrBuf h = StrBuf_Make(arena, req->Headers[i].Name.Length + req->Headers[i].Value.Length + 4);
    StrBuf_Append(&h, req->Headers[i].Name);
    StrBuf_AppendCStr(&h, ": ");
    StrBuf_Append(&h, req->Headers[i].Value);
    hdrs = curl_slist_append(hdrs, StrBuf_CStr(&h));
  }
  if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

  WriteCtx w = {};
  w.sink = sink;
  w.abort = abort;
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &w);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, XferCb);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &w);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "mini-agent/0.1");
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);

  CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK) {
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    res.Status = (i32)code;
  } else if (w.aborted) {
    res.Error = SV("aborted");
  } else {
    res.Error = StrView_Dup(arena, SV(curl_easy_strerror(rc)));
  }

  if (hdrs) curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  return res;
}

namespace {
bool FetchOnData(void* user, StrView chunk) {
  StrBuf_Append((StrBuf*)user, chunk);
  return true;
}
}  // namespace

HttpResponse Http_Fetch(Arena* arena, const HttpRequest* req, volatile i32* abort) {
  StrBuf buf = StrBuf_Make(arena, 1024);
  HttpStreamSink sink = {};
  sink.User = &buf;
  sink.OnData = FetchOnData;
  HttpResult r = Http_Stream(arena, req, sink, abort);
  HttpResponse res = {};
  res.Status = r.Status;
  res.Error = r.Error;
  res.Body = StrBuf_View(&buf);
  return res;
}
