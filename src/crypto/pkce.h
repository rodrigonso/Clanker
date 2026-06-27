#pragma once
#include "base/arena.h"
#include "base/str.h"
#include "crypto/base64.h"
#include "crypto/random.h"
#include "crypto/sha256.h"

// PKCE (RFC 7636) S256: verifier = base64url(32 random bytes),
// challenge = base64url(sha256(verifier)). Matches pi's flow.
struct Pkce {
  StrView Verifier;
  StrView Challenge;
};

inline Pkce Pkce_Generate(Arena* arena) {
  byte raw[32];
  RandomBytes(raw, 32);
  StrView verifier = Base64UrlEncode(arena, raw, 32);
  byte hash[32];
  Sha256((const byte*)verifier.Data, verifier.Length, hash);
  StrView challenge = Base64UrlEncode(arena, hash, 32);
  return Pkce{verifier, challenge};
}
