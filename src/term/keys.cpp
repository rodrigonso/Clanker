#include "term/keys.h"

#include <string.h>

void KeyDecoder_Init(KeyDecoder* d) {
  d->Len = 0;
  d->InPaste = false;
  d->PendingEscape = false;
}

void KeyDecoder_Push(KeyDecoder* d, const char* data, u32 n) {
  u32 room = (u32)sizeof(d->Buf) - d->Len;
  if (n > room) n = room;  // defensive: chunked reads keep us well under capacity
  memcpy(d->Buf + d->Len, data, n);
  d->Len += n;
}

static void Consume(KeyDecoder* d, u32 k) {
  if (k >= d->Len) {
    d->Len = 0;
  } else {
    memmove(d->Buf, d->Buf + k, d->Len - k);
    d->Len -= k;
  }
}

static void Clear(Key* out) {
  out->Kind = KeyKind::None;
  out->Codepoint = 0;
  out->Ctrl = false;
  out->Alt = false;
  out->Shift = false;
  out->MouseX = 0;
  out->MouseY = 0;
}

// Modifier params are 1 + bitmask on both the legacy and enhanced encodings.
static void ApplyMods(u32 mods, Key* out) {
  u32 m = mods >= 2 ? mods - 1 : 0;
  out->Shift = (m & 1) != 0;
  out->Alt = (m & 2) != 0;
  out->Ctrl = (m & 4) != 0;
}

// Map a legacy control byte, or a Ctrl+letter recovered from an enhanced key
// report, onto the shared key vocabulary. False for bytes with no binding.
static bool MapControlByte(u32 b, Key* out) {
  switch (b) {
    case 0x0d: out->Kind = KeyKind::Enter; return true;
    case 0x0a: out->Kind = KeyKind::Newline; return true;  // Ctrl+J
    case 0x7f:
    case 0x08: out->Kind = KeyKind::Backspace; return true;
    case 0x09: out->Kind = KeyKind::Tab; return true;
    case 0x03: out->Kind = KeyKind::CtrlC; return true;
    case 0x04: out->Kind = KeyKind::CtrlD; return true;
    case 0x0c: out->Kind = KeyKind::CtrlL; return true;
    case 0x0f: out->Kind = KeyKind::CtrlO; return true;
    case 0x15: out->Kind = KeyKind::CtrlU; return true;
    case 0x0b: out->Kind = KeyKind::CtrlK; return true;
    case 0x17: out->Kind = KeyKind::CtrlW; return true;
    case 0x01: out->Kind = KeyKind::Home; return true;  // Ctrl+A
    case 0x05: out->Kind = KeyKind::End; return true;   // Ctrl+E
    default: return false;
  }
}

const u32 kModsAlt = 3;  // the legacy meta prefix is exactly Alt
const u32 kKeypadEnter = 57414;

// Decode one key report carrying a code plus modifiers. Shared by the kitty and
// modifyOtherKeys encodings and by the legacy ESC-prefixed (Alt) form, so all
// three stay in agreement. Enabling either protocol re-encodes keys that were
// previously plain bytes -- Escape, Ctrl+letter, Alt+key -- so every binding
// has to survive this path.
static void MapEnhancedKey(u32 code, u32 mods, Key* out) {
  Clear(out);
  ApplyMods(mods, out);

  switch (code) {
    case 0x0d:
    case kKeypadEnter:
      out->Kind = (out->Shift || out->Alt) ? KeyKind::Newline : KeyKind::Enter;
      return;
    case 0x0a: out->Kind = KeyKind::Newline; return;
    case 0x1b: out->Kind = KeyKind::Escape; return;
    case 0x09: out->Kind = KeyKind::Tab; return;
    case 0x08:
    case 0x7f: out->Kind = out->Alt ? KeyKind::CtrlW : KeyKind::Backspace; return;
    default: break;
  }
  // Ctrl+Alt+<letter> must not alias onto the plain Ctrl binding, or
  // Ctrl+Alt+D would quit; the legacy form decoded to a dropped Alt'd Char.
  if (out->Ctrl && !out->Alt && code >= 'a' && code <= 'z') {
    if (!MapControlByte(code & 0x1f, out)) out->Kind = KeyKind::None;
    return;
  }
  if (out->Alt && !out->Ctrl) {
    if (code == 'b') { out->Kind = KeyKind::WordLeft; return; }
    if (code == 'f') { out->Kind = KeyKind::WordRight; return; }
  }
  // kitty reports functional keys in the private use area; they are not text.
  if (!out->Ctrl && code >= 0x20 && code <= 0x10ffff &&
      !(code >= 0xe000 && code <= 0xf8ff)) {
    out->Kind = KeyKind::Char;
    out->Codepoint = code;
  }
}

// Decode one UTF-8 scalar at the buffer front. Returns false if incomplete.
static bool DecodeUtf8(KeyDecoder* d, Key* out) {
  unsigned char c = (unsigned char)d->Buf[0];
  u32 need;
  u32 cp;
  if (c < 0x80) {
    need = 1;
    cp = c;
  } else if ((c >> 5) == 0x6) {
    need = 2;
    cp = c & 0x1f;
  } else if ((c >> 4) == 0xe) {
    need = 3;
    cp = c & 0x0f;
  } else if ((c >> 3) == 0x1e) {
    need = 4;
    cp = c & 0x07;
  } else {
    Consume(d, 1);
    out->Kind = KeyKind::None;
    return true;  // invalid lead byte, skip
  }
  if (d->Len < need) return false;  // wait for the rest
  for (u32 i = 1; i < need; i++) {
    unsigned char cc = (unsigned char)d->Buf[i];
    if ((cc >> 6) != 0x2) {
      Consume(d, 1);
      out->Kind = KeyKind::None;
      return true;  // malformed continuation, skip the lead
    }
    cp = (cp << 6) | (cc & 0x3f);
  }
  Consume(d, need);
  out->Kind = KeyKind::Char;
  out->Codepoint = cp;
  return true;
}

// Does the buffer start with `s` (or a proper prefix of it, when shorter)?
static bool StartsWithOrPrefix(KeyDecoder* d, const char* s, u32 slen) {
  u32 n = d->Len < slen ? d->Len : slen;
  return memcmp(d->Buf, s, n) == 0;
}

static bool ParseCsi(KeyDecoder* d, Key* out) {
  // d->Buf[0] == ESC, d->Buf[1] in { '[', 'O' }
  u32 i = 2;
  u32 p[4] = {0, 0, 0, 0};
  u32 np = 0;
  bool haveDigit = false;
  bool inSubParam = false;
  while (i < d->Len) {
    char ch = d->Buf[i];
    if (ch >= '0' && ch <= '9') {
      if (!inSubParam) {
        u32 idx = np < 4 ? np : 3;
        p[idx] = p[idx] * 10 + (u32)(ch - '0');
        haveDigit = true;
      }
      i++;
    } else if (ch == ';') {
      if (np < 3) np++;
      inSubParam = false;
      i++;
    } else if (ch == ':') {
      inSubParam = true;  // kitty sub-parameters carry no binding we use
      i++;
    } else {
      break;
    }
  }
  if (i >= d->Len) return false;  // no final byte yet
  char fin = d->Buf[i];
  u32 total = i + 1;
  u32 mod = (np >= 1 && haveDigit) ? p[1] : 0;  // second param carries modifiers

  // Enhanced keyboard reports: kitty sends CSI <code> ; <mods> u, xterm's
  // modifyOtherKeys sends CSI 27 ; <mods> ; <code> ~.
  if (fin == 'u' && haveDigit) {
    u32 code = p[0];
    // kitty reports the base layout key rather than the shifted character.
    if (mod == 2 && code >= 'a' && code <= 'z') code -= 'a' - 'A';
    MapEnhancedKey(code, mod, out);
    Consume(d, total);
    return true;
  }
  if (fin == '~' && haveDigit && np >= 2 && p[0] == 27) {
    MapEnhancedKey(p[2], p[1], out);
    Consume(d, total);
    return true;
  }

  KeyKind k = KeyKind::None;
  switch (fin) {
    case 'A': k = KeyKind::Up; break;
    case 'B': k = KeyKind::Down; break;
    case 'C': k = KeyKind::Right; break;
    case 'D': k = KeyKind::Left; break;
    case 'H': k = KeyKind::Home; break;
    case 'F': k = KeyKind::End; break;
    case '~':
      switch (p[0]) {
        case 1:
        case 7: k = KeyKind::Home; break;
        case 4:
        case 8: k = KeyKind::End; break;
        case 3: k = KeyKind::Delete; break;
        case 200: k = KeyKind::PasteStart; break;
        case 201: k = KeyKind::PasteEnd; break;
        default: k = KeyKind::None; break;
      }
      break;
    default: k = KeyKind::None; break;
  }

  Clear(out);
  out->Kind = k;
  if (mod >= 2) {
    ApplyMods(mod, out);
    if (out->Ctrl && k == KeyKind::Left) out->Kind = KeyKind::WordLeft;
    if (out->Ctrl && k == KeyKind::Right) out->Kind = KeyKind::WordRight;
  }
  Consume(d, total);
  if (k == KeyKind::PasteStart) d->InPaste = true;
  return true;
}

// Parse an SGR mouse event: ESC [ < Cb ; Cx ; Cy (M|m).
static bool ParseMouse(KeyDecoder* d, Key* out) {
  // d->Buf[0] == ESC, d->Buf[1] == '[', d->Buf[2] == '<'
  u32 i = 3;
  u32 p[3] = {0, 0, 0};
  bool have[3] = {false, false, false};
  u32 np = 0;
  while (i < d->Len) {
    char ch = d->Buf[i];
    if (ch >= '0' && ch <= '9') {
      u32 idx = np < 3 ? np : 2;
      p[idx] = p[idx] * 10 + (u32)(ch - '0');
      have[idx] = true;
      i++;
    } else if (ch == ';') {
      if (np < 2) np++;
      i++;
    } else {
      break;
    }
  }
  if (i >= d->Len) return false;  // final byte not yet arrived
  char fin = d->Buf[i];
  Clear(out);
  Consume(d, i + 1);
  if ((fin != 'M' && fin != 'm') || np != 2 || !have[0] || !have[1] || !have[2] ||
      p[1] == 0 || p[2] == 0)
    return true;

  u32 cb = p[0];
  out->MouseX = (u16)ClankMin(p[1] - 1, (u32)0xffff);
  out->MouseY = (u16)ClankMin(p[2] - 1, (u32)0xffff);
  out->Shift = (cb & 0x04) != 0;
  out->Alt = (cb & 0x08) != 0;
  out->Ctrl = (cb & 0x10) != 0;

  if (cb & 0x40) {  // mouse wheel (bit 6 set); low 2 bits give direction
    u32 dir = cb & 0x03;
    if (dir == 0)
      out->Kind = KeyKind::ScrollUp;
    else if (dir == 1)
      out->Kind = KeyKind::ScrollDown;
    // dir 2/3 (horizontal wheel) -> leave as None
  } else if (cb & 0x20) {
    out->Kind = KeyKind::MouseMove;
  } else if ((cb & 0x03) == 0) {
    out->Kind = fin == 'm' ? KeyKind::MouseLeftUp : KeyKind::MouseLeftDown;
  }
  return true;
}

bool KeyDecoder_Next(KeyDecoder* d, Key* out) {
  Clear(out);
  if (d->PendingEscape) {
    d->PendingEscape = false;
    out->Kind = KeyKind::Escape;
    return true;
  }
  if (d->Len == 0) return false;
  unsigned char c = (unsigned char)d->Buf[0];

  if (d->InPaste) {
    if (c == 0x1b) {
      if (StartsWithOrPrefix(d, "\x1b[201~", 6)) {
        if (d->Len < 6) return false;  // wait for the full end marker
        Consume(d, 6);
        d->InPaste = false;
        out->Kind = KeyKind::PasteEnd;
        return true;
      }
      Consume(d, 1);  // stray ESC inside paste -> drop
      out->Kind = KeyKind::None;
      return true;
    }
    if (c == '\r' || c == '\n') {
      Consume(d, 1);
      out->Kind = KeyKind::Newline;
      return true;
    }
    return DecodeUtf8(d, out);
  }

  if (c != 0x1b) {
    if (MapControlByte(c, out)) {
      Consume(d, 1);
      return true;
    }
    if (c < 0x20) {
      Consume(d, 1);
      out->Kind = KeyKind::None;
      return true;
    }
    return DecodeUtf8(d, out);
  }

  // ESC ...
  if (d->Len < 2) return false;  // lone ESC pends until the next byte
  unsigned char c1 = (unsigned char)d->Buf[1];
  if (c1 == 0x1b) {
    if (d->Len == 2) {
      Consume(d, 2);
      d->PendingEscape = true;
    } else {
      // Leave the second ESC to decode a following CSI/Alt sequence.
      Consume(d, 1);
    }
    out->Kind = KeyKind::Escape;
    return true;
  }
  if (c1 == '[' || c1 == 'O') {
    if (c1 == '[' && d->Len >= 3 && d->Buf[2] == '<') return ParseMouse(d, out);
    return ParseCsi(d, out);
  }
  // ESC + <char> is the legacy meta prefix, i.e. Alt+<char>.
  Consume(d, 2);
  MapEnhancedKey(c1, kModsAlt, out);
  return true;
}
