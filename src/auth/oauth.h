#pragma once
#include "base/arena.h"
#include "base/str.h"

// Provider-neutral pieces of an OAuth authorization-code flow with PKCE
// (RFC 7636): verifier, challenge and state generation, query and form
// encoding, and classification of the loopback redirect. All of it is pure.

struct OAuthPkce {
  StrView Verifier;
  StrView Challenge;  // base64url(SHA-256(Verifier)), method S256
  StrView State;
};

OAuthPkce OAuthPkce_Create(Arena* arena);
StrView OAuthPkce_Challenge(Arena* arena, StrView verifier);

// Appends "?name=value", or "&name=value" once the URL has a query,
// percent-encoding everything outside RFC 3986's unreserved set.
void OAuthQuery_Append(StrBuf* url, const char* name, StrView value);
// Appends an application/x-www-form-urlencoded field, '&'-separated.
void OAuthForm_Append(StrBuf* body, const char* name, StrView value);
// Finds the first `name` parameter in a query string (without the leading
// '?') and percent-decodes it into `arena`. A bare "name" yields an empty value.
bool OAuthQuery_Find(Arena* arena, StrView query, const char* name, StrView* out);

enum class OAuthCallbackStatus : u8 { Code, WrongPath, StateMismatch, Denied, MissingCode };

struct OAuthCallback {
  OAuthCallbackStatus Status;
  StrView Code;
  StrView Error;  // provider-supplied reason when Denied
};

// Classifies a loopback request-target such as "/callback?code=...&state=..."
// against the redirect `path` the provider was given.
OAuthCallback OAuthCallback_Parse(Arena* arena, StrView target, StrView path,
                                  StrView expectedState);
