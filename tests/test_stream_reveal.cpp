#include <string.h>

#include "base/arena.h"
#include "check.h"
#include "tui/stream_reveal.h"

namespace {

StrView AppendFrame(StreamReveal* reveal, StrBuf* out, u64 nowMs,
                    bool final = true) {
  StrView next = StreamReveal_NextFrame(reveal, nowMs, final);
  StrBuf_Append(out, next);
  return next;
}

}  // namespace

void TestStreamReveal() {
  Arena a = ArenaCreate();

  // A render frame drains every complete scalar received so far; no backlog
  // remains for an artificial typing-speed timer.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    char text[1024];
    for (u32 i = 0; i < sizeof(text); i++) text[i] = (char)('a' + (i % 26));
    StreamReveal_Push(&reveal, StrView{text, (u32)sizeof(text)});

    StrBuf out = StrBuf_Make(&a, sizeof(text) + 1);
    StrView frame = AppendFrame(&reveal, &out, 1000);
    CHECK_ALL(!StreamReveal_HasPending(&reveal),
              frame.Length == sizeof(text), out.Length == sizeof(text),
              memcmp(out.Data, text, sizeof(text)) == 0);
  }

  // Enqueues preserve exact order and never split a valid UTF-8 scalar.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    const char first[] = "A\xf0\x9f";
    const char second[] = "\x98\x80" "B";
    StreamReveal_Push(&reveal, StrView{first, (u32)sizeof(first) - 1});

    StrBuf out = StrBuf_Make(&a, 16);
    AppendFrame(&reveal, &out, 1000, false);
    CHECK_ALL(StrView_Eq(StrBuf_View(&out), SVL("A")),
              StreamReveal_NextFrame(&reveal, 1016, false).Length == 0);

    StreamReveal_Push(&reveal, StrView{second, (u32)sizeof(second) - 1});
    AppendFrame(&reveal, &out, 1032);
    CHECK_ALL(!StreamReveal_HasPending(&reveal),
              StrView_Eq(StrBuf_View(&out), SVL("A\xf0\x9f\x98\x80" "B")));
  }

  // Invalid and incomplete final UTF-8 still makes byte-for-byte progress.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    const char invalid[] = "\xf0(\x8c(";
    StreamReveal_Push(&reveal, StrView{invalid, (u32)sizeof(invalid) - 1});
    StrBuf out = StrBuf_Make(&a, 16);
    AppendFrame(&reveal, &out, 1000);
    CHECK_ALL(!StreamReveal_HasPending(&reveal),
              out.Length == sizeof(invalid) - 1,
              memcmp(out.Data, invalid, sizeof(invalid) - 1) == 0);

    const char incomplete[] = "\xe2\x82";
    StreamReveal_Push(&reveal, StrView{incomplete, (u32)sizeof(incomplete) - 1});
    StrBuf_Clear(&out);
    AppendFrame(&reveal, &out, 1016);
    CHECK_ALL(!StreamReveal_HasPending(&reveal),
              out.Length == sizeof(incomplete) - 1,
              memcmp(out.Data, incomplete, sizeof(incomplete) - 1) == 0);
  }

  // Deferred text buffers can be adopted without copying and the reveal state
  // remains reusable across assistant messages.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    StrBuf first = StrBuf_Make(&a, 16);
    StrBuf_Append(&first, SVL("first"));
    StreamReveal_Adopt(&reveal, first);
    StrBuf out = StrBuf_Make(&a, 16);
    AppendFrame(&reveal, &out, 1000);
    CHECK(!StreamReveal_HasPending(&reveal));
    StreamReveal_Push(&reveal, SVL("second"));
    AppendFrame(&reveal, &out, 1016);
    CHECK_ALL(!StreamReveal_HasPending(&reveal),
              StrView_Eq(StrBuf_View(&out), SVL("firstsecond")));
  }

  ArenaRelease(&a);
}
