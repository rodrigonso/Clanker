#pragma once
#include "base/str.h"

// One stored credential per provider. OpenRouter authenticates with a bearer
// API key, which `/auth` obtains through OAuth PKCE, so a credential is just
// that key.
struct Credential {
  StrView ApiKey;
};
