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
  byte bff[3] = {0xff, 0xff, 0xff};
  byte b1[1] = {0x66};
  CHECK_ALL(StrView_EqCStr(Base64UrlEncode(&a, b3, 3), "AAEC"),
            StrView_EqCStr(Base64UrlEncode(&a, bff, 3), "____"),
            StrView_EqCStr(Base64UrlEncode(&a, b1, 1), "Zg"));

  // Decode round-trips encode, for each padding-remainder case (0/1/2 extra
  // bytes past a full group of 3).
  StrView d3 = {};
  CHECK(Base64UrlDecode(&a, SVL("AAEC"), &d3));
  CHECK_ALL(d3.Length == 3, (byte)d3.Data[0] == 0, (byte)d3.Data[1] == 1, (byte)d3.Data[2] == 2);
  StrView dff = {};
  CHECK(Base64UrlDecode(&a, SVL("____"), &dff));
  CHECK_ALL(dff.Length == 3, (byte)dff.Data[0] == 0xff, (byte)dff.Data[1] == 0xff,
            (byte)dff.Data[2] == 0xff);
  StrView d1 = {};
  CHECK(Base64UrlDecode(&a, SVL("Zg"), &d1));
  CHECK_ALL(d1.Length == 1, (byte)d1.Data[0] == 0x66);

  // Tolerates '=' padding even though base64url/JWT segments are unpadded.
  StrView dPadded = {};
  CHECK(Base64UrlDecode(&a, SVL("Zg=="), &dPadded));
  CHECK(StrView_EqCStr(dPadded, "f"));

  // Empty input decodes to empty output (not malformed).
  StrView dEmpty = {};
  CHECK(Base64UrlDecode(&a, SVL(""), &dEmpty));
  CHECK(dEmpty.Length == 0);

  // Malformed: invalid length (mod 4 == 1 has no valid encoding).
  StrView bad = {};
  CHECK(!Base64UrlDecode(&a, SVL("A"), &bad));
  CHECK(bad.Length == 0);

  // Malformed: invalid characters (standard base64 '+'/'/' aren't base64url).
  CHECK(!Base64UrlDecode(&a, SVL("AA+C"), &bad));
  CHECK(!Base64UrlDecode(&a, SVL("AA/C"), &bad));
  CHECK(!Base64UrlDecode(&a, SVL("AA C"), &bad));

  // Encode -> decode round-trip over arbitrary bytes.
  byte mixed[5] = {0x00, 0x7f, 0x80, 0xab, 0xcd};
  StrView mixedEnc = Base64UrlEncode(&a, mixed, 5);
  StrView mixedDec = {};
  CHECK(Base64UrlDecode(&a, mixedEnc, &mixedDec));
  CHECK_ALL(mixedDec.Length == 5, memcmp(mixedDec.Data, mixed, 5) == 0);

  Pkce p = Pkce_Generate(&a);
  CHECK_ALL(p.Verifier.Length == 43, p.Challenge.Length == 43);
  byte vh[32];
  Sha256((const byte*)p.Verifier.Data, p.Verifier.Length, vh);
  CHECK(StrView_Eq(p.Challenge, Base64UrlEncode(&a, vh, 32)));
  Pkce p2 = Pkce_Generate(&a);
  CHECK(!StrView_Eq(p.Verifier, p2.Verifier));

  ArenaRelease(&a);
}
