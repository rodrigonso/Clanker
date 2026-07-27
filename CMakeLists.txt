cmake_minimum_required(VERSION 3.20)
project(Clank CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Convention: no C++ standard library, no exceptions, no RTTI.
if(MSVC)
  string(REGEX REPLACE "/EH[a-z]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  add_compile_options(/GR- /EHs-c- /W4 /permissive- /Zc:__cplusplus)
  add_compile_definitions(
    _HAS_EXCEPTIONS=0
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-fno-exceptions -fno-rtti -fno-threadsafe-statics -Wall -Wextra)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---- base ----------------------------------------------------------------
if(WIN32)
  set(CLANK_THREAD_SRC src/base/thread_win.cpp)
else()
  set(CLANK_THREAD_SRC src/base/thread_posix.cpp)
endif()
add_library(clank_base STATIC
  src/base/arena.cpp
  src/base/sysinfo.cpp
  ${CLANK_THREAD_SRC})
target_include_directories(clank_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(WIN32)
  target_link_libraries(clank_base PUBLIC psapi)
else()
  target_link_libraries(clank_base PUBLIC pthread)
endif()

# ---- json ----------------------------------------------------------------
add_library(clank_json STATIC
  src/json/json.cpp)
target_link_libraries(clank_json PUBLIC clank_base)

# ---- crypto --------------------------------------------------------------
add_library(clank_crypto STATIC
  src/crypto/sha256.cpp
  src/crypto/random.cpp)
target_link_libraries(clank_crypto PUBLIC clank_base)
if(WIN32)
  target_link_libraries(clank_crypto PUBLIC bcrypt)
endif()

# ---- agent ---------------------------------------------------------------
add_library(clank_agent STATIC
  src/agent/agent.cpp
  src/llm/retry.cpp)
target_link_libraries(clank_agent PUBLIC clank_base)

# ---- libcurl (vendored via FetchContent) ---------------------------------
include(FetchContent)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(HTTP_ONLY ON CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
if(WIN32)
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
elseif(APPLE)
  set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
else()
  set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(curl
  GIT_REPOSITORY https://github.com/curl/curl.git
  GIT_TAG curl-8_11_1
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(curl)

add_library(clank_http STATIC src/net/http_curl.cpp)
target_link_libraries(clank_http PUBLIC clank_base CURL::libcurl)

# ---- loopback (OAuth callback server + browser opener) -------------------
add_library(clank_loopback STATIC
  src/net/loopback.cpp
  src/net/browser.cpp)
target_link_libraries(clank_loopback PUBLIC clank_base)
if(WIN32)
  target_link_libraries(clank_loopback PUBLIC ws2_32 shell32)
endif()

# ---- term (full-screen TUI) ----------------------------------------------
if(WIN32)
  set(CLANK_TERM_PLATFORM src/term/term_win.cpp)
else()
  set(CLANK_TERM_PLATFORM src/term/term_posix.cpp)
endif()
add_library(clank_term STATIC
  src/term/term.cpp
  src/term/keys.cpp
  src/term/cell.cpp
  src/term/screen.cpp
  src/term/markdown.cpp
  src/term/sanitize.cpp
  src/term/stream_reveal.cpp
  src/term/linemodel.cpp
  src/term/toolview.cpp
  src/term/lineedit.cpp
  src/term/tui.cpp
  ${CLANK_TERM_PLATFORM})
target_link_libraries(clank_term PUBLIC clank_base clank_agent clank_json)

# ---- llm providers -------------------------------------------------------
add_library(clank_proto STATIC src/llm/anthropic_proto.cpp)
target_link_libraries(clank_proto PUBLIC clank_json)

add_library(clank_anthropic STATIC src/llm/anthropic.cpp)
target_link_libraries(clank_anthropic PUBLIC clank_proto clank_http)

# ---- auth (api keys + OAuth) ---------------------------------------------
add_library(clank_auth STATIC
  src/auth/store.cpp
  src/auth/oauth.cpp
  src/auth/resolve.cpp)
target_link_libraries(clank_auth PUBLIC clank_json clank_crypto clank_http clank_loopback)

# ---- tools ---------------------------------------------------------------
add_library(clank_tools STATIC
  src/tools/exec.cpp
  src/tools/fs.cpp
  src/tools/truncate.cpp
  src/tools/glob.cpp
  src/tools/regex.cpp
  src/tools/diff.cpp
  src/tools/filelock.cpp
  src/tools/read.cpp
  src/tools/write.cpp
  src/tools/edit.cpp
  src/tools/ls.cpp
  src/tools/find.cpp
  src/tools/grep.cpp
  src/tools/bash.cpp)
target_link_libraries(clank_tools PUBLIC clank_json clank_agent)

# ---- executables ---------------------------------------------------------
add_executable(clank src/app/main.cpp)
target_link_libraries(clank PRIVATE clank_anthropic clank_tools clank_agent clank_auth clank_term)

add_executable(clank_http_smoke src/app/http_smoke.cpp)
target_link_libraries(clank_http_smoke PRIVATE clank_http)

# ---- tests ---------------------------------------------------------------
enable_testing()
add_executable(clank_tests
  tests/test_main.cpp
  tests/test_base.cpp
  tests/test_types.cpp
  tests/test_json.cpp
  tests/test_agent.cpp
  tests/test_anthropic.cpp
  tests/test_tools.cpp
  tests/test_infra.cpp
  tests/test_crypto.cpp
  tests/test_auth.cpp
  tests/test_loopback.cpp
  tests/test_thread.cpp
  tests/test_keys.cpp
  tests/test_screen.cpp
  tests/test_markdown.cpp
  tests/test_sanitize.cpp
  tests/test_stream_reveal.cpp
  tests/test_linemodel.cpp
  tests/test_lineedit.cpp
  tests/test_tui.cpp)
target_link_libraries(clank_tests PRIVATE clank_proto clank_agent clank_tools clank_crypto clank_auth clank_loopback clank_term)
add_test(NAME unit COMMAND clank_tests)
