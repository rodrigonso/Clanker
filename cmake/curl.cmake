include(FetchContent)
set(CLANKER_CURL_ARCHIVE "" CACHE FILEPATH "Offline copy of the pinned curl release")
set(curl_version 8.11.1)
set(curl_hash c7ca7db48b0909743eaef34250da02c19bc61d4f1dcedd6603f109409536ab56)

# Keep dependency policy here, not in the platform wrappers.
foreach(option
    BUILD_CURL_EXE BUILD_SHARED_LIBS BUILD_TESTING BUILD_EXAMPLES
    BUILD_LIBCURL_DOCS BUILD_MISC_DOCS ENABLE_CURL_MANUAL CURL_ENABLE_EXPORT_TARGET
    PICKY_COMPILER CURL_STATIC_CRT CURL_USE_PKGCONFIG
    CURL_USE_LIBPSL CURL_USE_LIBSSH2 CURL_USE_LIBSSH CURL_USE_WOLFSSH
    CURL_USE_GSASL CURL_USE_GSSAPI CURL_USE_LIBUV USE_LIBRTMP USE_LIBIDN2
    CURL_ZLIB CURL_BROTLI CURL_ZSTD ENABLE_ARES
    USE_NGHTTP2 USE_NGTCP2 USE_QUICHE USE_MSH3 USE_OPENSSL_QUIC
    CURL_USE_OPENSSL CURL_USE_SCHANNEL CURL_USE_SECTRANSP
    CURL_USE_MBEDTLS CURL_USE_BEARSSL CURL_USE_WOLFSSL CURL_USE_GNUTLS CURL_USE_RUSTLS)
  set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(option BUILD_STATIC_LIBS HTTP_ONLY CURL_DISABLE_INSTALL CURL_DISABLE_WEBSOCKETS CURL_DISABLE_IPFS)
  set(${option} ON CACHE BOOL "" FORCE)
endforeach()
if(WIN32)
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
elseif(APPLE)
  set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
else()
  set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
  # FindOpenSSL caches library paths; changing the selection must find them anew.
  set(selection "${OPENSSL_ROOT_DIR}|${OPENSSL_USE_STATIC_LIBS}")
  if(DEFINED CLANKER_OPENSSL_SELECTION AND NOT CLANKER_OPENSSL_SELECTION STREQUAL selection)
    unset(OPENSSL_SSL_LIBRARY CACHE)
    unset(OPENSSL_CRYPTO_LIBRARY CACHE)
    unset(OPENSSL_INCLUDE_DIR CACHE)
  endif()
  set(CLANKER_OPENSSL_SELECTION "${selection}" CACHE INTERNAL "")
  if(OPENSSL_ROOT_DIR)
    if(NOT EXISTS "${OPENSSL_ROOT_DIR}/include/openssl/ssl.h")
      message(FATAL_ERROR "OpenSSL headers not found under ${OPENSSL_ROOT_DIR}/include")
    endif()
    # FindOpenSSL's explicit-root search omits multiarch library directories.
    set(saved_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
    if(OPENSSL_USE_STATIC_LIBS)
      set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
    endif()
    foreach(library SSL CRYPTO)
      string(TOLOWER "${library}" name)
      find_library(OPENSSL_${library}_LIBRARY NAMES "${name}"
        HINTS "${OPENSSL_ROOT_DIR}"
        PATH_SUFFIXES "lib/${CMAKE_LIBRARY_ARCHITECTURE}" lib lib64
        NO_DEFAULT_PATH REQUIRED)
    endforeach()
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${saved_suffixes}")
  endif()
  find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
  if(OPENSSL_ROOT_DIR AND CMAKE_LIBRARY_ARCHITECTURE)
    set(multiarch_include "${OPENSSL_ROOT_DIR}/include/${CMAKE_LIBRARY_ARCHITECTURE}")
    if(IS_DIRECTORY "${multiarch_include}")
      foreach(target OpenSSL::SSL OpenSSL::Crypto)
        target_include_directories(${target} SYSTEM INTERFACE "${multiarch_include}")
      endforeach()
    endif()
  endif()
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
