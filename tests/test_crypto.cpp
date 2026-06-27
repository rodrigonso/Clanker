#include <string.h>

#include "base/arena.h"
#include "check.h"
#include "crypto/base64.h"
#include "crypto/pkce.h"
#include "crypto/sha256.h"

static void HexOf(const byte h[32], char out[65]) {
  static const char* hx = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out[i * 2] = hx[h[i] >> 4];
    out[i * 2 + 1] = hx[h[i] & 0xF];
  }
  out[64] = '\0';
}

void TestCrypto() {
  byte h[32];
  char hex[65];

  Sha256((const byte*)"abc", 3, h);
  HexOf(h, hex);
  CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

  Sha256((const byte*)"", 0, h);
  HexOf(h, hex);
  CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

  // 56 bytes: forces padding into a second block.
  const char* msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  Sha256((const byte*)msg, strlen(msg), h);
  HexOf(h, hex);
  CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);

  Arena a = ArenaCreate();
  byte b3[3] = {0, 1, 2};
  CHECK(StrView_EqCStr(Base64UrlEncode(&a, b3, 3), "AAEC"));
  byte bff[3] = {0xff, 0xff, 0xff};
  CHECK(StrView_EqCStr(Base64UrlEncode(&a, bff, 3), "____"));
  byte b1[1] = {0x66};
  CHECK(StrView_EqCStr(Base64UrlEncode(&a, b1, 1), "Zg"));

  Pkce p = Pkce_Generate(&a);
  CHECK(p.Verifier.Length == 43);
  CHECK(p.Challenge.Length == 43);
  byte vh[32];
  Sha256((const byte*)p.Verifier.Data, p.Verifier.Length, vh);
  CHECK(StrView_Eq(p.Challenge, Base64UrlEncode(&a, vh, 32)));
  Pkce p2 = Pkce_Generate(&a);
  CHECK(!StrView_Eq(p.Verifier, p2.Verifier));

  ArenaRelease(&a);
}
