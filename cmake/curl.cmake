include(FetchContent)
set(CLANKER_CURL_ARCHIVE "" CACHE FILEPATH "Offline copy of the pinned curl release")
set(curl_version 8.11.1)
set(curl_hash c7ca7db48b0909743eaef34250da02c19bc61d4f1dcedd6603f109409536ab56)

# Keep dependency policy here, not in the platform wrappers.
foreach(option
    BUILD_CURL_EXE BUILD_SHARED_LIBS BUILD_TESTING BUILD_EXAMPLES
    BUILD_LIBCURL_DOCS BUILD_MISC_DOCS ENABLE_CURL_MANUAL CURL_ENABLE_EXPORT_TARGET
    PICKY_COMPILER CURL_USE_PKGCONFIG
    CURL_USE_LIBPSL CURL_USE_LIBSSH2 CURL_USE_LIBSSH CURL_USE_WOLFSSH
    CURL_USE_GSASL CURL_USE_GSSAPI CURL_USE_LIBUV USE_LIBRTMP USE_LIBIDN2
    CURL_ZLIB CURL_BROTLI CURL_ZSTD ENABLE_ARES
    USE_NGHTTP2 USE_NGTCP2 USE_QUICHE USE_MSH3 USE_OPENSSL_QUIC
    CURL_USE_OPENSSL CURL_USE_SCHANNEL CURL_USE_SECTRANSP
    CURL_USE_MBEDTLS CURL_USE_BEARSSL CURL_USE_WOLFSSL CURL_USE_GNUTLS CURL_USE_RUSTLS)
  set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
# Inherit Clanker's config-specific CRT policy; curl's switch also forces /MTd
# in Debug, which must keep /MDd to match the rest of the build.
set(CURL_STATIC_CRT OFF CACHE BOOL "" FORCE)
foreach(option BUILD_STATIC_LIBS HTTP_ONLY CURL_DISABLE_INSTALL CURL_DISABLE_WEBSOCKETS CURL_DISABLE_IPFS)
  set(${option} ON CACHE BOOL "" FORCE)
endforeach()
if(WIN32)
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
elseif(APPLE)
  set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
else()
  message(FATAL_ERROR "Clanker requires native Windows or macOS TLS.")
endif()

set(curl_url "https://curl.se/download/curl-${curl_version}.tar.xz")
if(CLANKER_CURL_ARCHIVE)
  if(NOT EXISTS "${CLANKER_CURL_ARCHIVE}")
    message(FATAL_ERROR "Curl archive does not exist: ${CLANKER_CURL_ARCHIVE}")
  endif()
  # Verify on every configure, even when FetchContent already populated sources.
  file(SHA256 "${CLANKER_CURL_ARCHIVE}" archive_hash)
  if(NOT archive_hash STREQUAL curl_hash)
    message(FATAL_ERROR "Curl archive SHA256 mismatch: ${CLANKER_CURL_ARCHIVE}")
  endif()
  set(curl_url "${CLANKER_CURL_ARCHIVE}")
endif()
FetchContent_Declare(curl URL "${curl_url}" URL_HASH "SHA256=${curl_hash}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(curl)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/clanker-curl.txt"
  CONTENT "${curl_SOURCE_DIR}\n${curl_version}\n${curl_hash}\n")
