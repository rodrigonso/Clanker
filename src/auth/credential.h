#pragma once
#include "base/str.h"

enum class CredKind : u8 { None, ApiKey, OAuth };

// One stored credential per provider. OAuth holds access/refresh tokens,
// optional identity metadata, and expiry.
struct Credential {
  CredKind Kind;
  StrView ApiKey;   // ApiKey
  StrView Access;   // OAuth access token
  StrView Refresh;  // OAuth refresh token
  u64 ExpiresMs;    // OAuth absolute epoch-ms expiry (with safety margin baked in)
  StrView IdToken;   // OAuth identity token, when issued
  StrView AccountId; // Provider account/workspace identifier, when required
  bool IsFedRamp;    // Provider account requires FedRAMP request routing
};
