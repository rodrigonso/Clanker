#include <string.h>

#include "base/arena.h"
#include "check.h"
#include "crypto/base64.h"

void TestCrypto() {
  Arena a = ArenaCreate();

  byte b3[3] = {0, 1, 2};
  byte bff[3] = {0xff, 0xff, 0xff};
  byte b1[1] = {0x66};
  byte b2[2] = {0x66, 0x6f};
  CHECK_ALL(StrView_EqCStr(Base64Encode(&a, b3, 3), "AAEC"),
            StrView_EqCStr(Base64Encode(&a, bff, 3), "////"),
            StrView_EqCStr(Base64Encode(&a, b1, 1), "Zg=="),
            StrView_EqCStr(Base64Encode(&a, b2, 2), "Zm8="));

  CHECK(Base64Encode(&a, b3, 0).Length == 0);

  // 5 bytes exercises a full group plus a 2-byte remainder, and covers the
  // high-bit values that a signed char would sign-extend.
  byte mixed[5] = {0x00, 0x7f, 0x80, 0xab, 0xcd};
  StrView encoded = Base64Encode(&a, mixed, 5);
  CHECK_ALL(encoded.Length == 8, StrView_EqCStr(encoded, "AH+Aq80="));

  ArenaRelease(&a);
}
