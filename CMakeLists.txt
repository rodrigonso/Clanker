cmake_minimum_required(VERSION 3.20)
project(Clanker CXX)

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
  set(CLANKER_THREAD_SRC src/base/thread_win.cpp)
else()
  set(CLANKER_THREAD_SRC src/base/thread_posix.cpp)
endif()
add_library(clanker_base STATIC
  src/base/arena.cpp
  src/base/config_file.cpp
  src/base/process_file_lock.cpp
  src/base/sysinfo.cpp
  ${CLANKER_THREAD_SRC})
target_include_directories(clanker_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(WIN32)
  target_link_libraries(clanker_base PUBLIC psapi)
else()
  target_link_libraries(clanker_base PUBLIC pthread)
endif()

# ---- json ----------------------------------------------------------------
add_library(clanker_json STATIC
  src/json/json.cpp
  src/json/schema.cpp)
target_link_libraries(clanker_json PUBLIC clanker_base)

# ---- crypto --------------------------------------------------------------
add_library(clanker_crypto STATIC
  src/crypto/random.cpp)
target_link_libraries(clanker_crypto PUBLIC clanker_base)
if(WIN32)
  target_link_libraries(clanker_crypto PUBLIC bcrypt)
endif()

# ---- durable sessions ---------------------------------------------------
add_library(clanker_session STATIC
  src/session/id.cpp
  src/session/entry.cpp
  src/session/metadata.cpp
  src/session/reducer.cpp
  src/session/memory_repository.cpp
  src/session/jsonl_repository.cpp)
target_link_libraries(clanker_session PUBLIC clanker_base clanker_json clanker_crypto)

# ---- agent ---------------------------------------------------------------
add_library(clanker_agent STATIC
  src/agent/harness.cpp
  src/agent/driver.cpp
  src/agent/primitives.cpp
  src/agent/permissions.cpp
  src/agent/compaction.cpp
  src/agent/event_json.cpp
  src/llm/retry.cpp)
target_link_libraries(clanker_agent PUBLIC clanker_base clanker_session)

# ---- libcurl (vendored via FetchContent) ---------------------------------
include(FetchContent)
set(CLANKER_FETCHCONTENT_CACHE_ROOT
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
  DOWNLOAD_DIR "${CLANKER_FETCHCONTENT_CACHE_ROOT}/downloads"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(curl)

add_library(clanker_http STATIC src/net/http_curl.cpp)
target_link_libraries(clanker_http PUBLIC clanker_base CURL::libcurl)

# ---- term (terminal I/O + cell renderer) ---------------------------------
if(WIN32)
  set(CLANKER_TERM_PLATFORM src/term/term_win.cpp)
else()
  set(CLANKER_TERM_PLATFORM src/term/term_posix.cpp)
endif()
add_library(clanker_term STATIC
  src/term/term.cpp
  src/term/keys.cpp
  src/term/cell.cpp
  src/term/screen.cpp
  src/term/sanitize.cpp
  ${CLANKER_TERM_PLATFORM})
target_link_libraries(clanker_term PUBLIC clanker_base)

# ---- ui (reusable terminal component library) ----------------------------
add_library(clanker_ui STATIC
  src/ui/lineedit.cpp
  src/ui/ui.cpp
  src/ui/layout.cpp
  src/ui/canvas.cpp
  src/ui/event.cpp
  src/ui/widgets.cpp)
target_link_libraries(clanker_ui PUBLIC clanker_base clanker_term)

# ---- tui (Clanker-specific full-screen application) ----------------------
add_library(clanker_tui STATIC
  src/tui/markdown.cpp
  src/tui/modal.cpp
  src/tui/perf.cpp
  src/tui/selection.cpp
  src/tui/session.cpp
  src/tui/status.cpp
  src/tui/stream_reveal.cpp
  src/tui/linemodel.cpp
  src/tui/toolview.cpp
  src/tui/tui.cpp)
# The TUI is a terminal component layer: it takes progress as ordinary calls and
# asks for steer/cancel through TuiAgentHooks, so it does not link clanker_agent.
# src/app/tui_bridge.cpp is the only place that knows about both.
target_link_libraries(clanker_tui PUBLIC clanker_ui clanker_term clanker_json clanker_proto)

# ---- llm providers -------------------------------------------------------
add_library(clanker_proto STATIC
  src/llm/responses_proto.cpp)
target_link_libraries(clanker_proto PUBLIC clanker_json)

add_library(clanker_responses STATIC src/llm/responses.cpp)
target_link_libraries(clanker_responses PUBLIC clanker_proto clanker_http clanker_auth)

# ---- auth (api keys) -----------------------------------------------------
add_library(clanker_auth STATIC
  src/auth/store.cpp
  src/auth/resolve.cpp)
target_link_libraries(clanker_auth PUBLIC clanker_json)

# ---- tools ---------------------------------------------------------------
add_library(clanker_tools STATIC
  src/tools/exec.cpp
  src/tools/tools_manager.cpp
  src/tools/tool_select.cpp
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
  src/tools/ask_user.cpp
  src/tools/shell.cpp)
target_link_libraries(clanker_tools PUBLIC clanker_json clanker_agent clanker_http)

# ---- app support (settings, provider registry, interactive commands) ------
add_library(clanker_app STATIC
  src/app/auth_flow.cpp
  src/app/command.cpp
  src/app/commands.cpp
  src/app/config_paths.cpp
  src/app/event_pump.cpp
  src/app/frontmatter.cpp
  src/app/hook_config.cpp
  src/app/hook_gate.cpp
  src/app/hook_runner.cpp
  src/app/plain_select.cpp
  src/app/project_trust.cpp
  src/app/prompt_context.cpp
  src/app/prompt_templates.cpp
  src/app/provider_registry.cpp
  src/app/runtime.cpp
  src/app/session_switch.cpp
  src/app/settings.cpp
  src/app/skills.cpp
  src/app/tui_bridge.cpp)
target_link_libraries(clanker_app PUBLIC clanker_responses clanker_auth clanker_tui clanker_tools)

# ---- executables ---------------------------------------------------------
add_executable(clanker src/app/main.cpp)
target_link_libraries(clanker PRIVATE clanker_app clanker_responses clanker_tools clanker_agent clanker_auth clanker_tui)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT clanker)
set_target_properties(clanker PROPERTIES
  VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# ---- tests ---------------------------------------------------------------
# One harness for everything: tests/test_main.cpp is a CLI that selects suites
# by name, --filter, or --all. There are deliberately no other test binaries.
enable_testing()
add_executable(clanker_tests EXCLUDE_FROM_ALL
  tests/test_main.cpp
  tests/benchmark_agent.cpp
  tests/test_base.cpp
  tests/test_types.cpp
  tests/test_json.cpp
  tests/test_session.cpp
  tests/test_harness.cpp
  tests/test_permissions.cpp
  tests/test_responses.cpp
  tests/test_tools.cpp
  tests/test_tool_select.cpp
  tests/test_infra.cpp
  tests/test_crypto.cpp
  tests/test_auth.cpp
  tests/test_command.cpp
  tests/test_app.cpp
  tests/test_hooks.cpp
  tests/test_prompt_context.cpp
  tests/test_project_trust.cpp
  tests/test_provider_registry.cpp
  tests/test_settings.cpp
  tests/test_skills.cpp
  tests/test_prompt_templates.cpp
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
target_link_libraries(clanker_tests PRIVATE clanker_app clanker_responses clanker_proto clanker_agent clanker_session clanker_tools clanker_crypto clanker_auth clanker_tui clanker_ui clanker_term clanker_http)

# Default ctest run stays offline; the network suite is opt-in.
add_test(NAME unit COMMAND clanker_tests)
add_test(NAME network COMMAND clanker_tests http_live)
set_tests_properties(network PROPERTIES LABELS network DISABLED TRUE)
