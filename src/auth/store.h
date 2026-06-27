#pragma once
#include "auth/credential.h"
#include "base/arena.h"
#include "base/str.h"

// Credential storage in a JSON file keyed by provider id, one credential each.
// Writes are atomic (temp + rename) with restrictive perms on POSIX.

// Default path: <home>/.mini-agent/auth.json
StrView AuthStore_DefaultPath(Arena* arena);

bool AuthStore_Read(Arena* arena, StrView path, StrView providerId, Credential* out);
bool AuthStore_Write(Arena* arena, StrView path, StrView providerId, const Credential* cred);
bool AuthStore_Delete(Arena* arena, StrView path, StrView providerId);
