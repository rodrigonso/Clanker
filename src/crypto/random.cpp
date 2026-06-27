#include "crypto/random.h"

#if defined(_WIN32)
#include <windows.h>
// bcrypt.h must follow windows.h
#include <bcrypt.h>

void RandomBytes(byte* out, u32 len) {
  BCryptGenRandom(nullptr, out, len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

#else
#include <stdio.h>

void RandomBytes(byte* out, u32 len) {
  FILE* f = fopen("/dev/urandom", "rb");
  if (f) {
    size_t got = fread(out, 1, len, f);
    (void)got;
    fclose(f);
  }
}
#endif
