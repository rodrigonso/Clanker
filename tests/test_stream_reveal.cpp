#include <string.h>

#include "base/arena.h"
#include "check.h"
#include "tui/stream_reveal.h"

namespace {

StrView AppendFrame(StreamReveal* reveal, StrBuf* out, u64 nowMs,
                    bool blocked = false) {
  StrView next = StreamReveal_NextFrame(reveal, nowMs, blocked);
  StrBuf_Append(out, next);
  return next;
}

// Step frames on a fixed cadence until the reveal drains or `maxFrames` pass.
// Returns the number of frames spent, so a test can assert that pacing happened
// without pinning the exact per-frame split.
u32 RunFrames(StreamReveal* reveal, StrBuf* out, u64 startMs, u64 stepMs,
              u32 maxFrames, bool blocked = false) {
  u32 frames = 0;
  u64 now = startMs;
  while (frames < maxFrames && StreamReveal_HasPending(reveal)) {
    AppendFrame(reveal, out, now, blocked);
    frames++;
    now += stepMs;
  }
  return frames;
}

StrView FillAscii(Arena* a, u32 length) {
  char* text = (char*)ArenaAlloc(a, length, 1);
  for (u32 i = 0; i < length; i++) text[i] = (char)('a' + (i % 26));
  return StrView{text, length};
}

}  // namespace

void TestStreamReveal() {
  Arena a = ArenaCreate();

  // Text is paced across frames rather than dumped whole, and every byte still
  // arrives in order.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    StrView text = FillAscii(&a, 1024);
    StreamReveal_Push(&reveal, text);

    StrBuf out = StrBuf_Make(&a, text.Length + 1);
    StrView first = AppendFrame(&reveal, &out, 1000);
    CHECK_ALL(first.Length > 0, first.Length < text.Length,
              StreamReveal_HasPending(&reveal));

    u32 frames = RunFrames(&reveal, &out, 1016, 16, 4096);
    CHECK_ALL(!StreamReveal_HasPending(&reveal), frames > 1,
              out.Length == text.Length,
              memcmp(out.Data, text.Data, text.Length) == 0);
  }

  // The controller is proportional: a deeper backlog reveals more per frame
  // than a shallow one over the same elapsed time.
  {
    StreamReveal shallow;
    StreamReveal deep;
    StreamReveal_Init(&shallow, &a);
    StreamReveal_Init(&deep, &a);
    StreamReveal_Push(&shallow, FillAscii(&a, 24));
    StreamReveal_Push(&deep, FillAscii(&a, 4096));

    StreamReveal_NextFrame(&shallow, 1000, false);
    StreamReveal_NextFrame(&deep, 1000, false);
    StrView shallowFrame = StreamReveal_NextFrame(&shallow, 1016, false);
    StrView deepFrame = StreamReveal_NextFrame(&deep, 1016, false);
    CHECK(deepFrame.Length > shallowFrame.Length);
  }

  // Reveal rate stays under the hard ceiling across a one-second window even
  // when the backlog is far larger than the window could drain.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    StreamReveal_Push(&reveal, FillAscii(&a, 100 * 1024));

    StrBuf out = StrBuf_Make(&a, 8192);
    for (u64 now = 1000; now <= 2000; now += 16) AppendFrame(&reveal, &out, now);
    CHECK_ALL(out.Length >= 800, out.Length <= 1024,
              StreamReveal_HasPending(&reveal));
  }

  // A frame's budget scales with elapsed time, so a long frame interval reveals
  // proportionally more. The previous pacing attempt capped this at four
  // scalars per frame, which silently became a hard rate ceiling that no
  // backlog could overcome; the cap must not come back.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    StreamReveal_Push(&reveal, FillAscii(&a, 8192));

    StreamReveal_NextFrame(&reveal, 1000, false);
    StrView wide = StreamReveal_NextFrame(&reveal, 1050, false);
    CHECK(wide.Length >= 40);
  }

  // Enqueues preserve exact order and never split a valid UTF-8 scalar.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    const char first[] = "A\xf0\x9f";
    const char second[] = "\x98\x80" "B";
    StreamReveal_Push(&reveal, StrView{first, (u32)sizeof(first) - 1});

    StrBuf out = StrBuf_Make(&a, 16);
    AppendFrame(&reveal, &out, 1000);
    CHECK_ALL(StrView_Eq(StrBuf_View(&out), SVL("A")),
              StreamReveal_NextFrame(&reveal, 1016, false).Length == 0);

    StreamReveal_Push(&reveal, StrView{second, (u32)sizeof(second) - 1});
    RunFrames(&reveal, &out, 1032, 16, 64);
    CHECK_ALL(!StreamReveal_HasPending(&reveal),
              StrView_Eq(StrBuf_View(&out), SVL("A\xf0\x9f\x98\x80" "B")));
  }

  // A budget boundary lands on a scalar boundary even when every scalar is
  // multibyte, so no frame can emit a partial glyph.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    const u32 kScalars = 512;
    char* text = (char*)ArenaAlloc(&a, kScalars * 3, 1);
    for (u32 i = 0; i < kScalars; i++) {
      text[i * 3 + 0] = '\xe2';
      text[i * 3 + 1] = '\x82';
      text[i * 3 + 2] = '\xac';  // U+20AC EURO SIGN
    }
    StrView euros{text, kScalars * 3};
    StreamReveal_Push(&reveal, euros);

    StrBuf out = StrBuf_Make(&a, euros.Length + 1);
    u64 now = 1000;
    bool aligned = true;
    while (StreamReveal_HasPending(&reveal)) {
      StrView frame = AppendFrame(&reveal, &out, now);
      if (frame.Length % 3 != 0) aligned = false;
      now += 16;
    }
    CHECK_ALL(aligned, out.Length == euros.Length,
              memcmp(out.Data, euros.Data, euros.Length) == 0);
  }

  // Invalid and incomplete trailing UTF-8 still makes byte-for-byte progress
  // once nothing more can arrive to complete it.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    const char invalid[] = "\xf0(\x8c(";
    StreamReveal_Push(&reveal, StrView{invalid, (u32)sizeof(invalid) - 1});
    StrBuf out = StrBuf_Make(&a, 16);
    RunFrames(&reveal, &out, 1000, 16, 64, true);
    CHECK_ALL(!StreamReveal_HasPending(&reveal),
              out.Length == sizeof(invalid) - 1,
              memcmp(out.Data, invalid, sizeof(invalid) - 1) == 0);

    const char incomplete[] = "\xe2\x82";
    StreamReveal_Push(&reveal, StrView{incomplete, (u32)sizeof(incomplete) - 1});
    StrBuf_Clear(&out);
    RunFrames(&reveal, &out, 1064, 16, 64, true);
    CHECK_ALL(!StreamReveal_HasPending(&reveal),
              out.Length == sizeof(incomplete) - 1,
              memcmp(out.Data, incomplete, sizeof(incomplete) - 1) == 0);
  }

  // Blocking on the reveal drains the whole backlog within kRevealDeadlineMs,
  // so a display event queued behind it waits a bounded time no matter how long
  // the reply is.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    StrView text = FillAscii(&a, 64 * 1024);
    StreamReveal_Push(&reveal, text);

    StrBuf out = StrBuf_Make(&a, text.Length + 1);
    for (u64 now = 1000; now <= 1000 + kRevealDeadlineMs; now += 16)
      AppendFrame(&reveal, &out, now, true);
    CHECK_ALL(!StreamReveal_HasPending(&reveal), out.Length == text.Length,
              memcmp(out.Data, text.Data, text.Length) == 0);
  }

  // Staying blocked re-arms every frame but cannot push the tail out, and text
  // pushed after the deadline is armed still lands inside it.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    StreamReveal_Push(&reveal, FillAscii(&a, 4096));

    StrBuf out = StrBuf_Make(&a, 8192);
    for (u64 now = 1000; now <= 1000 + kRevealDeadlineMs; now += 16) {
      StreamReveal_Push(&reveal, SVL("tail"));
      AppendFrame(&reveal, &out, now, true);
    }
    CHECK(!StreamReveal_HasPending(&reveal));
  }

  // A deadline already in the past drains everything on the next frame.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    StrView text = FillAscii(&a, 4096);
    StreamReveal_Push(&reveal, text);

    StrBuf out = StrBuf_Make(&a, text.Length + 1);
    AppendFrame(&reveal, &out, 1000, true);
    AppendFrame(&reveal, &out, 1000 + kRevealDeadlineMs + 1, true);
    CHECK_ALL(!StreamReveal_HasPending(&reveal), out.Length == text.Length);
  }

  // Deferred text buffers can be adopted without copying. Adopting resets the
  // pacing clock, so the next message is paced from scratch instead of
  // inheriting the credit the previous one had banked.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    StrBuf first = StrBuf_Make(&a, 16);
    StrBuf_Append(&first, SVL("first"));
    StreamReveal_Adopt(&reveal, first);
    StrBuf out = StrBuf_Make(&a, 4096);
    RunFrames(&reveal, &out, 1000, 16, 64, true);
    CHECK(!StreamReveal_HasPending(&reveal));

    StrBuf second = StrBuf_Make(&a, 2048);
    StrBuf_Append(&second, FillAscii(&a, 1024));
    StreamReveal_Adopt(&reveal, second);
    StrView paced = AppendFrame(&reveal, &out, 2000);
    CHECK_ALL(paced.Length > 0, paced.Length < second.Length,
              StreamReveal_HasPending(&reveal));
    RunFrames(&reveal, &out, 2016, 16, 4096);
    CHECK_ALL(!StreamReveal_HasPending(&reveal),
              out.Length == 5 + second.Length);
  }

  // A stalled or backwards clock neither stalls the reveal permanently nor
  // unlocks unbounded output.
  {
    StreamReveal reveal;
    StreamReveal_Init(&reveal, &a);
    StrView text = FillAscii(&a, 4096);
    StreamReveal_Push(&reveal, text);

    StrBuf out = StrBuf_Make(&a, text.Length + 1);
    for (u32 i = 0; i < 64; i++) AppendFrame(&reveal, &out, 1000);
    u32 stalled = out.Length;
    CHECK_ALL(stalled > 0, stalled < text.Length,
              StreamReveal_HasPending(&reveal));

    // Time moving backwards credits nothing rather than wrapping.
    for (u32 i = 0; i < 8; i++) AppendFrame(&reveal, &out, 900 - i);
    CHECK(out.Length == stalled);

    // Progress resumes once the clock advances again.
    RunFrames(&reveal, &out, 2000, 16, 4096);
    CHECK_ALL(!StreamReveal_HasPending(&reveal), out.Length == text.Length,
              memcmp(out.Data, text.Data, text.Length) == 0);
  }

  ArenaRelease(&a);
}
