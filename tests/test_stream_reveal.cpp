#include <string.h>

#include "base/arena.h"
#include "check.h"
#include "tui/stream_reveal.h"

namespace {

StrView AppendFrame(StreamReveal* reveal, StrBuf* out, u64 nowMs) {
  StrView next = StreamReveal_NextFrame(reveal, nowMs, true);
  StrBuf_Append(out, next);
  return next;
}

bool Drain(StreamReveal* reveal, StrBuf* out, u64* nowMs, u32 maxFrameLength = 0) {
  u32 frames = 0;
  while (StreamReveal_HasPending(reveal) && frames < 1000) {
    StrView frame = AppendFrame(reveal, out, *nowMs);
    if (frame.Length == 0 || (maxFrameLength != 0 && frame.Length > maxFrameLength)) return false;
    *nowMs += 16;
    frames++;
  }
  return !StreamReveal_HasPending(reveal);
}

}  // namespace

void TestStreamReveal() {
  Arena a = ArenaCreate();

  // Large bursts remain continuous: every frame is capped at four ASCII
  // scalars instead of growing in proportion to the backlog.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    char text[1024];
    for (u32 i = 0; i < sizeof(text); i++) text[i] = (char)('a' + (i % 26));
    StreamReveal_Push(&reveal, StrView{text, (u32)sizeof(text)});

    StrBuf out = StrBuf_Make(&a, sizeof(text) + 1);
    u64 now = 1000;
    bool drained = Drain(&reveal, &out, &now, 4);
    CHECK_ALL(drained, out.Length == sizeof(text),
              memcmp(out.Data, text, sizeof(text)) == 0);
  }

  // A large backlog accelerates smoothly within the same four-scalar cap.
  {
    StreamReveal small;
    StreamReveal large;
    StreamReveal_Init(&small, &a);
    StreamReveal_Init(&large, &a);
    StreamReveal_Push(&small, SVL("hello"));

    char text[1600];
    memset(text, 'x', sizeof(text));
    StreamReveal_Push(&large, StrView{text, (u32)sizeof(text)});

    CHECK_ALL(StreamReveal_NextFrame(&small, 1000, true).Length == 1,
              StreamReveal_NextFrame(&large, 1000, true).Length == 1);
    StrView smallFrame = StreamReveal_NextFrame(&small, 1016, true);
    StrView largeFrame = StreamReveal_NextFrame(&large, 1016, true);
    CHECK_ALL(smallFrame.Length >= 1, largeFrame.Length > smallFrame.Length,
              largeFrame.Length <= 4);
  }

  // Enqueues preserve exact order and never split a valid UTF-8 scalar.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    const char first[] = "A\xf0\x9f";
    const char second[] = "\x98\x80" "B";
    StreamReveal_Push(&reveal, StrView{first, (u32)sizeof(first) - 1});

    StrBuf out = StrBuf_Make(&a, 16);
    StrView firstFrame = StreamReveal_NextFrame(&reveal, 1000, false);
    StrBuf_Append(&out, firstFrame);
    CHECK_ALL(StrView_Eq(StrBuf_View(&out), SVL("A")),
              StreamReveal_NextFrame(&reveal, 1016, false).Length == 0);

    StreamReveal_Push(&reveal, StrView{second, (u32)sizeof(second) - 1});
    u64 now = 1032;
    bool drained = Drain(&reveal, &out, &now);
    CHECK_ALL(drained,
              StrView_Eq(StrBuf_View(&out), SVL("A\xf0\x9f\x98\x80" "B")));
  }

  // Invalid and incomplete final UTF-8 still makes paced byte-for-byte progress.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    const char invalid[] = "\xf0(\x8c(";
    StreamReveal_Push(&reveal, StrView{invalid, (u32)sizeof(invalid) - 1});
    StrBuf out = StrBuf_Make(&a, 16);
    u64 now = 1000;
    bool drained = Drain(&reveal, &out, &now);
    CHECK_ALL(drained, out.Length == sizeof(invalid) - 1,
              memcmp(out.Data, invalid, sizeof(invalid) - 1) == 0);

    const char incomplete[] = "\xe2\x82";
    StreamReveal_Push(&reveal, StrView{incomplete, (u32)sizeof(incomplete) - 1});
    StrBuf_Clear(&out);
    drained = Drain(&reveal, &out, &now);
    CHECK_ALL(drained, out.Length == sizeof(incomplete) - 1,
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
    u64 now = 1000;
    CHECK(Drain(&reveal, &out, &now));
    StreamReveal_Push(&reveal, SVL("second"));
    bool drained = Drain(&reveal, &out, &now);
    CHECK_ALL(drained,
              StrView_Eq(StrBuf_View(&out), SVL("firstsecond")));
  }

  ArenaRelease(&a);
}
