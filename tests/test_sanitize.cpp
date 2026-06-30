#include "base/arena.h"
#include "check.h"
#include "term/sanitize.h"

namespace {

// Sanitize `in` into a fresh arena buffer and return a stable view over it.
StrView San(Arena* a, StrView in) {
  StrBuf b = StrBuf_Make(a, 64);
  Sanitize_Display(&b, in);
  return StrBuf_View(&b);
}
StrView SanC(Arena* a, const char* s) { return San(a, SV(s)); }

bool AllSpaces(StrView s, u32 from, u32 to) {
  for (u32 i = from; i < to; i++)
    if (s.Data[i] != ' ') return false;
  return true;
}

}  // namespace

void TestSanitize() {
  Arena a = ArenaCreate();

  // Empty input -> empty output.
  CHECK(San(&a, StrView{nullptr, 0}).Length == 0);

  // Plain text is untouched.
  CHECK(StrView_EqCStr(SanC(&a, "hello world"), "hello world"));

  // --- carriage returns (the Windows CRLF artifact trigger) ---
  CHECK(StrView_EqCStr(SanC(&a, "a\r\nb\r\n"), "a\nb\n"));  // CRLF -> LF
  CHECK(StrView_EqCStr(SanC(&a, "x\ry"), "xy"));            // bare CR dropped
  CHECK(StrView_EqCStr(SanC(&a, "line\r"), "line"));        // trailing CR dropped
  CHECK(StrView_EqCStr(SanC(&a, "a\nb"), "a\nb"));          // lone LF preserved

  // A dir-style multi-line CRLF block: no \r survives, lines split on \n only.
  StrView dir = SanC(&a, " Volume Serial Number is 6C52-1E84\r\n Directory of C:\\x\r\n");
  CHECK(StrView_EqCStr(dir, " Volume Serial Number is 6C52-1E84\n Directory of C:\\x\n"));

  // --- tab expansion to 8-column stops ---
  {
    StrView s = SanC(&a, "\tx");  // col 0 -> pad 8
    CHECK(s.Length == 9);
    CHECK(AllSpaces(s, 0, 8) && s.Data[8] == 'x');
  }
  {
    StrView s = SanC(&a, "a\tb");  // 'a' at col 0 -> col 1, pad 7
    CHECK(s.Length == 9);
    CHECK(s.Data[0] == 'a' && AllSpaces(s, 1, 8) && s.Data[8] == 'b');
  }
  {
    StrView s = SanC(&a, "ab\n\tc");  // tab stop resets after newline -> pad 8
    CHECK(s.Length == 12);
    CHECK(s.Data[2] == '\n' && AllSpaces(s, 3, 11) && s.Data[11] == 'c');
  }

  // --- ANSI / VT escape sequences (CSI + OSC) ---
  CHECK(StrView_EqCStr(SanC(&a, "\033[31mred\033[0m"), "red"));         // SGR color
  CHECK(StrView_EqCStr(SanC(&a, "\033[2J\033[Hhi"), "hi"));             // erase + cursor home
  CHECK(StrView_EqCStr(SanC(&a, "\033[38;5;244mX\033[0m"), "X"));       // 256-color params
  CHECK(StrView_EqCStr(SanC(&a, "\033]0;window title\007Z"), "Z"));     // OSC, BEL-terminated
  CHECK(StrView_EqCStr(SanC(&a, "\033]8;;http://x\033\\link\033]8;;\033\\"), "link"));  // OSC hyperlink, ST
  CHECK(StrView_EqCStr(SanC(&a, "ab\033"), "ab"));                      // lone trailing ESC dropped
  CHECK(StrView_EqCStr(SanC(&a, "a\033cb"), "acb"));                    // bare ESC dropped, 'c' kept (pi parity)

  // --- other control characters ---
  CHECK(StrView_EqCStr(SanC(&a, "a\001\002b"), "ab"));  // C0 controls dropped
  CHECK(StrView_EqCStr(SanC(&a, "a\177b"), "ab"));      // DEL dropped
  {
    const char raw[] = {'a', '\0', 'b'};  // embedded NUL dropped
    CHECK(StrView_EqCStr(San(&a, StrView{raw, 3}), "ab"));
  }

  // --- UTF-8 is preserved verbatim ---
  CHECK(StrView_EqCStr(SanC(&a, "h\303\251llo"), "h\303\251llo"));  // "héllo"

  // A wide (width-2) glyph advances the tab-stop column by 2.
  {
    StrView s = SanC(&a, "\344\270\255\tx");  // "中" + tab + x ; col 2 -> pad 6
    CHECK(s.Length == 10);
    CHECK((unsigned char)s.Data[0] == 0xE4 && (unsigned char)s.Data[1] == 0xB8 &&
          (unsigned char)s.Data[2] == 0xAD);
    CHECK(AllSpaces(s, 3, 9) && s.Data[9] == 'x');
  }

  ArenaRelease(&a);
}
