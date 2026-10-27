#pragma once
#include "base/basics.h"

#define SHA256_DIGEST_SIZE 32

// SHA-256 of `data` through the platform's crypto library.
void Sha256(const byte* data, u32 len, byte out[SHA256_DIGEST_SIZE]);
