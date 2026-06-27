#pragma once
#include "base/basics.h"

// SHA-256 of a buffer. Writes 32 raw bytes into `out`.
void Sha256(const byte* data, usize len, byte out[32]);
