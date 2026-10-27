#pragma once
#include "base/str.h"

// True under SSH, where a browser on the user's own machine cannot reach a
// listener bound to this machine's loopback interface.
bool Browser_IsRemoteSession();

// Opens `url` in the user's default browser. Returns false when no browser
// could be launched.
bool Browser_Open(StrView url);
