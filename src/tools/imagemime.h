#pragma once
#include "base/basics.h"
#include "base/str.h"

// Image MIME detection for the read tool. Two strategies: sniff magic bytes
// (most reliable) and map a file extension. Both return an empty StrView when
// the input is not a recognized image type.

inline StrView Img_SniffMime(const byte* d, u32 n) {
  if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G' && d[4] == 0x0D &&
      d[5] == 0x0A && d[6] == 0x1A && d[7] == 0x0A)
    return SVL("image/png");
  if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return SVL("image/jpeg");
  if (n >= 6 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F' && d[3] == '8' &&
      (d[4] == '7' || d[4] == '9') && d[5] == 'a')
    return SVL("image/gif");
  if (n >= 12 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' && d[8] == 'W' &&
      d[9] == 'E' && d[10] == 'B' && d[11] == 'P')
    return SVL("image/webp");
  if (n >= 2 && d[0] == 'B' && d[1] == 'M') return SVL("image/bmp");
  return StrView{nullptr, 0};
}

// `ext` is a file extension slice including the leading dot, any case.
inline StrView Img_MimeForExt(StrView ext) {
  if (StrView_EqInsensitiveAscii(ext, SVL(".png"))) return SVL("image/png");
  if (StrView_EqInsensitiveAscii(ext, SVL(".jpg")) ||
      StrView_EqInsensitiveAscii(ext, SVL(".jpeg"))) return SVL("image/jpeg");
  if (StrView_EqInsensitiveAscii(ext, SVL(".gif"))) return SVL("image/gif");
  if (StrView_EqInsensitiveAscii(ext, SVL(".webp"))) return SVL("image/webp");
  if (StrView_EqInsensitiveAscii(ext, SVL(".bmp"))) return SVL("image/bmp");
  return StrView{nullptr, 0};
}
