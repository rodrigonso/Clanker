#include "term/sanitize.h"

#include "term/cell.h"

namespace {

const u32 kTabStop = 8;

bool InRange(unsigned char c, unsigned char lo, unsigned char hi) { return c >= lo && c <= hi; }

// Consume one ANSI/VT escape sequence starting at `i` (in.Data[i] == 0x1B).
// Returns the index just past the sequence. Only CSI (`ESC [ ...`) and OSC
// (`ESC ] ...`) are recognized as full sequences; any other ESC-led byte drops
// just the ESC so the following byte is processed normally -- matching pi, whose
// stripAnsi removes only CSI/OSC and leaves a bare ESC to be dropped as a
// control character.
u32 ConsumeEscape(StrView in, u32 i) {
  u32 n = in.Length;
  u32 j = i + 1;         // first byte after ESC
  if (j >= n) return j;  // lone trailing ESC -> drop it
  unsigned char c = (unsigned char)in.Data[j];

  if (c == '[') {  // CSI: ESC [ <param/intermediate 0x20-0x3F>* <final 0x40-0x7E>
    j++;
    while (j < n && InRange((unsigned char)in.Data[j], 0x20, 0x3F)) j++;
    if (j < n && InRange((unsigned char)in.Data[j], 0x40, 0x7E)) j++;
    return j;
  }
  if (c == ']') {  // OSC: ESC ] ... terminated by BEL, ST (ESC \), or C1 ST (0x9C)
    j++;
    while (j < n) {
      unsigned char d = (unsigned char)in.Data[j];
      if (d == 0x07 || d == 0x9C) {  // BEL or 8-bit ST
        j++;
        break;
      }
      if (d == 0x1B && j + 1 < n && in.Data[j + 1] == '\\') {  // 7-bit ST
        j += 2;
        break;
      }
      j++;
    }
    return j;
  }
  return j;  // any other ESC-led byte: drop only the ESC (the byte after is kept)
}

}  // namespace

void Sanitize_Display(StrBuf* out, StrView in) {
  u32 col = 0;  // display column since the last newline (for tab stops)
  u32 i = 0;
  while (i < in.Length) {
    if ((unsigned char)in.Data[i] == 0x1B) {  // strip the whole escape sequence
      i = ConsumeEscape(in, i);
      continue;
    }

    u32 j = i;
    u32 cp = Utf8Decode(in, &j);  // decode one scalar, advancing j

    if (cp == 0x0A) {  // newline: keep, reset tab-stop column
      StrBuf_AppendChar(out, '\n');
      col = 0;
      i = j;
      continue;
    }
    if (cp == 0x09) {  // tab: expand to the next 8-column tab stop
      u32 pad = kTabStop - (col % kTabStop);
      for (u32 k = 0; k < pad; k++) StrBuf_AppendChar(out, ' ');
      col += pad;
      i = j;
      continue;
    }
    if (cp < 0x20 || cp == 0x7F) {  // carriage return + other C0 / DEL: drop
      i = j;
      continue;
    }

    StrBuf_Append(out, StrView{in.Data + i, j - i});  // keep the scalar verbatim
    col += CharWidth(cp);
    i = j;
  }
}
