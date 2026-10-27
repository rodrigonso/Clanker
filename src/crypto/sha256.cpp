#include "crypto/sha256.h"

#if defined(_WIN32)
#include <windows.h>
// bcrypt.h must follow windows.h
#include <bcrypt.h>

void Sha256(const byte* data, u32 len, byte out[SHA256_DIGEST_SIZE]) {
  NTSTATUS status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                               (PUCHAR)data, len, out, SHA256_DIGEST_SIZE);
  FFI(!BCRYPT_SUCCESS(status));
}

#else
#include <CommonCrypto/CommonDigest.h>

void Sha256(const byte* data, u32 len, byte out[SHA256_DIGEST_SIZE]) {
  CC_SHA256(data, (CC_LONG)len, out);
}
#endif
