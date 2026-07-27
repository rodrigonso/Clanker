#include "term/keys.h"

#include "check.h"

namespace {

bool g_allKeysAvailable;

// Push a C-string of bytes into the decoder.
void Push(KeyDecoder* d, const char* s) {
  u32 n = 0;
  while (s[n]) n++;
  KeyDecoder_Push(d, s, n);
}

// Pull the next key. Callers validate the decoded value.
Key Next(KeyDecoder* d) {
  Key k = {};
  bool available = KeyDecoder_Next(d, &k);
  g_allKeysAvailable = available && g_allKeysAvailable;
  return k;
}

bool NextKind(KeyDecoder* d, KeyKind kind) { return Next(d).Kind == kind; }

bool NextCodepoint(KeyDecoder* d, u32 codepoint) {
  Key key = Next(d);
  return key.Kind == KeyKind::Char && key.Codepoint == codepoint;
}

}  // namespace

void TestKeys() {
  g_allKeysAvailable = true;
  KeyDecoder d;

  // Plain ASCII characters.
  KeyDecoder_Init(&d);
  Push(&d, "abc");
  Key k = {};
  CHECK(NextCodepoint(&d, 'a') && NextCodepoint(&d, 'b') &&
        NextCodepoint(&d, 'c'));
  CHECK(!KeyDecoder_Next(&d, &k));  // drained

  // Enter vs newline.
  KeyDecoder_Init(&d);
  Push(&d, "\r");
  CHECK(Next(&d).Kind == KeyKind::Enter);
  Push(&d, "\n");
  CHECK(Next(&d).Kind == KeyKind::Newline);

  // Control keys + backspace.
  KeyDecoder_Init(&d);
  Push(&d, "\x7f\x03\x17\x0f");
  CHECK(NextKind(&d, KeyKind::Backspace) && NextKind(&d, KeyKind::CtrlC) &&
        NextKind(&d, KeyKind::CtrlW) && NextKind(&d, KeyKind::CtrlO));

  // Consecutive Escape bytes are two standalone presses, including when an
  // Escape is immediately followed by an arrow sequence.
  KeyDecoder_Init(&d);
  Push(&d, "\x1b\x1b");
  CHECK(Next(&d).Kind == KeyKind::Escape);
  CHECK(Next(&d).Kind == KeyKind::Escape);
  CHECK(!KeyDecoder_Next(&d, &k));
  Push(&d, "\x1b\x1b[A");
  CHECK(Next(&d).Kind == KeyKind::Escape);
  CHECK(Next(&d).Kind == KeyKind::Up);

  // Arrows + Home/End + Delete.
  KeyDecoder_Init(&d);
  Push(&d, "\x1b[A\x1b[B\x1b[C\x1b[D");
  CHECK(NextKind(&d, KeyKind::Up) && NextKind(&d, KeyKind::Down) &&
        NextKind(&d, KeyKind::Right) && NextKind(&d, KeyKind::Left));
  Push(&d, "\x1b[H\x1b[F\x1b[3~");
  CHECK(NextKind(&d, KeyKind::Home) && NextKind(&d, KeyKind::End) &&
        NextKind(&d, KeyKind::Delete));

  // Ctrl+Right / Ctrl+Left -> word navigation.
  KeyDecoder_Init(&d);
  Push(&d, "\x1b[1;5C\x1b[1;5D");
  Key wr = Next(&d);
  CHECK(wr.Kind == KeyKind::WordRight && wr.Ctrl);
  CHECK(Next(&d).Kind == KeyKind::WordLeft);

  // Shift+Ctrl word selection.
  KeyDecoder_Init(&d);
  Push(&d, "\x1b[1;6C\x1b[1;6D");
  Key swr = Next(&d);
  CHECK(swr.Kind == KeyKind::WordRight && swr.Shift && swr.Ctrl);
  CHECK(Next(&d).Kind == KeyKind::WordLeft);

  // Modifier parsing: Shift+Up keeps the Up kind but flags Shift.
  KeyDecoder_Init(&d);
  Push(&d, "\x1b[1;2A");
  Key su = Next(&d);
  CHECK(su.Kind == KeyKind::Up && su.Shift);

  // Alt+Enter and Alt+b/f.
  KeyDecoder_Init(&d);
  Push(&d, "\x1b\r");
  CHECK(Next(&d).Kind == KeyKind::Newline);
  Push(&d, "\x1b""b\x1b""f");
  CHECK(Next(&d).Kind == KeyKind::WordLeft);
  CHECK(Next(&d).Kind == KeyKind::WordRight);

  // UTF-8 multibyte: 'é' = 0xC3 0xA9.
  KeyDecoder_Init(&d);
  Push(&d, "\xc3\xa9");
  Key e = Next(&d);
  CHECK(e.Kind == KeyKind::Char && e.Codepoint == 0xe9);

  // Incomplete sequence split across two pushes.
  KeyDecoder_Init(&d);
  Push(&d, "\x1b[");
  CHECK(!KeyDecoder_Next(&d, &k));  // incomplete
  Push(&d, "A");
  CHECK(Next(&d).Kind == KeyKind::Up);

  // Split UTF-8 across pushes.
  KeyDecoder_Init(&d);
  Push(&d, "\xc3");
  CHECK(!KeyDecoder_Next(&d, &k));
  Push(&d, "\xa9");
  CHECK(Next(&d).Codepoint == 0xe9);

  // Bracketed paste: newline inside is a Newline, not Enter.
  KeyDecoder_Init(&d);
  Push(&d, "\x1b[200~ab\ncd\x1b[201~");
  CHECK(NextKind(&d, KeyKind::PasteStart) && NextCodepoint(&d, 'a') &&
        NextCodepoint(&d, 'b') && NextKind(&d, KeyKind::Newline) &&
        NextCodepoint(&d, 'c') && NextCodepoint(&d, 'd') &&
        NextKind(&d, KeyKind::PasteEnd));
  CHECK(!KeyDecoder_Next(&d, &k));

  // SGR mouse: wheel up/down map to ScrollUp/ScrollDown.
  KeyDecoder_Init(&d);
  Push(&d, "\x1b[<64;10;5M");
  Key wheelUp = Next(&d);
  CHECK_ALL(wheelUp.Kind == KeyKind::ScrollUp,
            wheelUp.MouseX == 9 && wheelUp.MouseY == 4);
  Push(&d, "\x1b[<65;10;5M");
  CHECK(Next(&d).Kind == KeyKind::ScrollDown);

  // Modifiers on a wheel event still scroll (Shift+wheel-up: Cb = 64 + 4).
  KeyDecoder_Init(&d);
  Push(&d, "\x1b[<68;1;1M");
  Key shiftedWheel = Next(&d);
  CHECK(shiftedWheel.Kind == KeyKind::ScrollUp && shiftedWheel.Shift);

  // Left press/release preserve zero-based coordinates.
  KeyDecoder_Init(&d);
  Push(&d, "\x1b[<0;3;4M\x1b[<0;3;4m");
  Key mouseDown = Next(&d);
  CHECK_ALL(mouseDown.Kind == KeyKind::MouseLeftDown,
            mouseDown.MouseX == 2 && mouseDown.MouseY == 3);
  Key mouseUp = Next(&d);
  CHECK_ALL(mouseUp.Kind == KeyKind::MouseLeftUp,
            mouseUp.MouseX == 2 && mouseUp.MouseY == 3);
  CHECK(!KeyDecoder_Next(&d, &k));  // drained

  // All-motion tracking reports hover movement, including button drags.
  KeyDecoder_Init(&d);
  Push(&d, "\x1b[<35;8;6M\x1b[<60;9;7M");
  Key move = Next(&d);
  CHECK_ALL(move.Kind == KeyKind::MouseMove, move.MouseX == 7 && move.MouseY == 5);
  Key drag = Next(&d);
  CHECK_ALL(drag.Kind == KeyKind::MouseMove, drag.MouseX == 8 && drag.MouseY == 6,
            drag.Shift && drag.Alt && drag.Ctrl);

  // Unsupported buttons and malformed zero coordinates are consumed.
  KeyDecoder_Init(&d);
  Push(&d, "\x1b[<2;1;1M\x1b[<2;1;1m\x1b[<0;0;0M");
  CHECK(NextKind(&d, KeyKind::None) && NextKind(&d, KeyKind::None) &&
        NextKind(&d, KeyKind::None));

  // Horizontal wheel (Cb 66/67) is recognised but ignored.
  KeyDecoder_Init(&d);
  Push(&d, "\x1b[<66;1;1M");
  CHECK(Next(&d).Kind == KeyKind::None);

  // A mouse sequence split across pushes pends until the final byte arrives.
  KeyDecoder_Init(&d);
  Push(&d, "\x1b[<64;10;");
  CHECK(!KeyDecoder_Next(&d, &k));
  Push(&d, "5M");
  CHECK(Next(&d).Kind == KeyKind::ScrollUp);
  CHECK(g_allKeysAvailable);
}
