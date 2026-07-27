#pragma once
#include "base/arena.h"
#include "base/str.h"

// Resolve a usable token for a provider. Priority: stored api_key -> stored OAuth
// (auto-refresh + persist if near expiry) -> environment variable. Returns an
// empty view if nothing is configured. Called per turn so OAuth tokens refresh
// mid-session. Optional abort applies to refresh transport.
StrView Auth_ResolveToken(Arena* arena, StrView authPath, StrView providerId, const char* envVar,
                          volatile i32* abort = nullptr);
