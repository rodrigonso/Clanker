#pragma once

#include "base/basics.h"
#include "base/str.h"

enum class SessionStatus : u8 {
  Ok,
  InvalidArgument,
  NotFound,
  Busy,
  Corrupt,
  UnsupportedVersion,
  IoError,
  Faulted,
  Conflict,
  InvalidState,
  Cancelled,
  ProviderError,
  HookError,
  WatcherLagged,
};

struct SessionResult {
  SessionStatus Status;
  StrView Message;
  u64 Seq;
};

inline SessionResult SessionResult_Ok(u64 seq = 0) {
  return SessionResult{SessionStatus::Ok, StrView{"", 0}, seq};
}

inline SessionResult SessionResult_Error(SessionStatus status, StrView message, u64 seq = 0) {
  return SessionResult{status, message, seq};
}

inline bool SessionResult_IsOk(SessionResult result) {
  return result.Status == SessionStatus::Ok;
}
