cmake_minimum_required(VERSION 3.20)
project(Clank CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
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
  src/base/config_file.cpp
  src/base/process_file_lock.cpp
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
  src/json/json.cpp
  src/json/schema.cpp)
target_link_libraries(clank_json PUBLIC clank_base)

# ---- crypto --------------------------------------------------------------
add_library(clank_crypto STATIC
  src/crypto/sha256.cpp
  src/crypto/random.cpp)
target_link_libraries(clank_crypto PUBLIC clank_base)
if(WIN32)
  target_link_libraries(clank_crypto PUBLIC bcrypt)
endif()

# ---- durable sessions ---------------------------------------------------
add_library(clank_session STATIC
  src/session/id.cpp
  src/session/entry.cpp
  src/session/metadata.cpp
  src/session/reducer.cpp
  src/session/memory_repository.cpp
  src/session/jsonl_repository.cpp)
target_link_libraries(clank_session PUBLIC clank_base clank_json clank_crypto)

# ---- agent ---------------------------------------------------------------
add_library(clank_agent STATIC
  src/agent/harness.cpp
  src/agent/driver.cpp
  src/agent/primitives.cpp
  src/agent/compaction.cpp
  src/agent/event_json.cpp
  src/llm/retry.cpp)
target_link_libraries(clank_agent PUBLIC clank_base clank_session)

# ---- libcurl (vendored via FetchContent) ---------------------------------
include(FetchContent)
set(CLANK_FETCHCONTENT_CACHE_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/out/fetchcontent"
  CACHE PATH "Persistent download cache for vendored dependencies")
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)
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
  URL https://curl.se/download/curl-8.11.1.tar.xz
  URL_HASH SHA256=c7ca7db48b0909743eaef34250da02c19bc61d4f1dcedd6603f109409536ab56
  DOWNLOAD_DIR "${CLANK_FETCHCONTENT_CACHE_ROOT}/downloads"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
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

# ---- term (terminal I/O + cell renderer) ---------------------------------
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
  src/term/sanitize.cpp
  ${CLANK_TERM_PLATFORM})
target_link_libraries(clank_term PUBLIC clank_base)

# ---- ui (reusable terminal component library) ----------------------------
add_library(clank_ui STATIC
  src/ui/lineedit.cpp
  src/ui/ui.cpp
  src/ui/layout.cpp
  src/ui/canvas.cpp
  src/ui/event.cpp
  src/ui/widgets.cpp)
target_link_libraries(clank_ui PUBLIC clank_base clank_term)

# ---- tui (Clank-specific full-screen application) ------------------------
add_library(clank_tui STATIC
  src/tui/markdown.cpp
  src/tui/stream_reveal.cpp
  src/tui/linemodel.cpp
  src/tui/toolview.cpp
  src/tui/tui.cpp)
target_link_libraries(clank_tui PUBLIC clank_ui clank_term clank_agent clank_json)

# ---- llm providers -------------------------------------------------------
add_library(clank_proto STATIC
  src/llm/anthropic_proto.cpp
  src/llm/openai_proto.cpp)
target_link_libraries(clank_proto PUBLIC clank_json)

add_library(clank_anthropic STATIC src/llm/anthropic.cpp)
target_link_libraries(clank_anthropic PUBLIC clank_proto clank_http)

add_library(clank_openai STATIC src/llm/openai.cpp)
target_link_libraries(clank_openai PUBLIC clank_proto clank_http clank_auth)

# ---- auth (api keys + OAuth) ---------------------------------------------
add_library(clank_auth STATIC
  src/auth/store.cpp
  src/auth/oauth.cpp
  src/auth/resolve.cpp)
target_link_libraries(clank_auth PUBLIC clank_json clank_crypto clank_http clank_loopback)

# ---- tools ---------------------------------------------------------------
add_library(clank_tools STATIC
  src/tools/exec.cpp
  src/tools/tools_manager.cpp
  src/tools/fs.cpp
  src/tools/truncate.cpp
  src/tools/glob.cpp
  src/tools/diff.cpp
  src/tools/filelock.cpp
  src/tools/read.cpp
  src/tools/write.cpp
  src/tools/edit.cpp
  src/tools/ls.cpp
  src/tools/find.cpp
  src/tools/grep.cpp
  src/tools/shell.cpp)
target_link_libraries(clank_tools PUBLIC clank_json clank_agent clank_http)

# ---- app support (settings, provider registry, interactive commands) ------
add_library(clank_app STATIC
  src/app/command.cpp
  src/app/plain_select.cpp
  src/app/project_trust.cpp
  src/app/prompt_context.cpp
  src/app/provider_registry.cpp
  src/app/settings.cpp)
target_link_libraries(clank_app PUBLIC clank_anthropic clank_openai clank_auth clank_tui clank_tools)

# ---- executables ---------------------------------------------------------
add_executable(clank src/app/main.cpp)
target_link_libraries(clank PRIVATE clank_app clank_anthropic clank_openai clank_tools clank_agent clank_auth clank_tui)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT clank)
set_target_properties(clank PROPERTIES
  VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# ---- tests ---------------------------------------------------------------
# One harness for everything: tests/test_main.cpp is a CLI that selects suites
# by name, --filter, or --all. There are deliberately no other test binaries.
enable_testing()
add_executable(clank_tests EXCLUDE_FROM_ALL
  tests/test_main.cpp
  tests/benchmark_agent.cpp
  tests/test_base.cpp
  tests/test_types.cpp
  tests/test_json.cpp
  tests/test_session.cpp
  tests/test_harness.cpp
  tests/test_anthropic.cpp
  tests/test_openai.cpp
  tests/test_tools.cpp
  tests/test_infra.cpp
  tests/test_crypto.cpp
  tests/test_auth.cpp
  tests/test_command.cpp
  tests/test_prompt_context.cpp
  tests/test_project_trust.cpp
  tests/test_loopback.cpp
  tests/test_provider_registry.cpp
  tests/test_settings.cpp
  tests/test_compaction.cpp
  tests/test_event_json.cpp
  tests/test_thread.cpp
  tests/test_keys.cpp
  tests/test_screen.cpp
  tests/test_markdown.cpp
  tests/test_sanitize.cpp
  tests/test_stream_reveal.cpp
  tests/test_linemodel.cpp
  tests/test_lineedit.cpp
  tests/test_ui.cpp
  tests/test_ui_widgets.cpp
  tests/test_ui_demo.cpp
  tests/test_tui.cpp
  tests/test_http.cpp)
target_link_libraries(clank_tests PRIVATE clank_app clank_proto clank_agent clank_session clank_tools clank_crypto clank_auth clank_loopback clank_tui clank_ui clank_term clank_http)

# Default ctest run stays offline; the network suite is opt-in.
add_test(NAME unit COMMAND clank_tests)
add_test(NAME network COMMAND clank_tests http_live)
set_tests_properties(network PROPERTIES LABELS network DISABLED TRUE)
