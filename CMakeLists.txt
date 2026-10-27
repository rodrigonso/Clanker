cmake_minimum_required(VERSION 3.24)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CLANKER_VERSION LIMIT_COUNT 1)
project(Clanker VERSION "${CLANKER_VERSION}" LANGUAGES NONE)
if(NOT CMAKE_HOST_SYSTEM_NAME MATCHES "^(Windows|Darwin)$" OR
   NOT CMAKE_SYSTEM_NAME MATCHES "^(Windows|Darwin)$")
  message(FATAL_ERROR "Clanker supports only Windows and macOS (host: ${CMAKE_HOST_SYSTEM_NAME}, target: ${CMAKE_SYSTEM_NAME}).")
endif()
enable_language(C CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION)

option(CLANKER_BUILD_CLI "Build the CLI and its networking dependencies" ON)
option(CLANKER_BUILD_TESTS "Build the offline-by-default test runner" OFF)
option(CLANKER_BUILD_EXAMPLES "Build the offline core example" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:DebugDLL>")
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()
foreach(kind RUNTIME ARCHIVE LIBRARY)
  # Keep paths identical for single- and multi-configuration generators.
  set(CMAKE_${kind}_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<0:>")
endforeach()

if(MSVC)
  string(REGEX REPLACE " /W[0-4]| /GR| /EH[a-z-]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
else()
  # C++ objects use only the C runtime; do not add a C++ runtime at link time.
  list(REMOVE_ITEM CMAKE_CXX_IMPLICIT_LINK_LIBRARIES stdc++ c++ c++abi)
endif()

function(clanker_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /GR- /EHs-c- /W4 /permissive- /Zc:__cplusplus /utf-8)
    target_compile_definitions(${target} PRIVATE
      _HAS_EXCEPTIONS=0 NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE
      -fno-exceptions -fno-rtti -fno-threadsafe-statics -Wall -Wextra)
    set_target_properties(${target} PROPERTIES LINKER_LANGUAGE C)
  endif()
endfunction()

add_library(clanker_core STATIC
  src/base/arena.cpp
  src/base/config_file.cpp
  src/base/file_io.cpp
  src/base/process_file_lock.cpp
  src/base/sysinfo.cpp
  src/json/json.cpp
  src/json/schema.cpp
  src/tools/fs.cpp
  src/tools/glob.cpp
  src/crypto/random.cpp
  src/crypto/sha256.cpp
  src/session/id.cpp
  src/session/entry.cpp
  src/session/metadata.cpp
  src/session/reducer.cpp
  src/session/memory_repository.cpp
  src/session/jsonl_repository.cpp
  src/llm/retry.cpp
  src/agent/harness.cpp
  src/agent/driver.cpp
  src/agent/primitives.cpp
  src/agent/permissions.cpp
  src/agent/compaction.cpp
  src/agent/event_json.cpp)
clanker_target(clanker_core)
set_target_properties(clanker_core PROPERTIES DEBUG_POSTFIX d)
target_compile_features(clanker_core PUBLIC cxx_std_23)
target_include_directories(clanker_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(WIN32)
  target_sources(clanker_core PRIVATE src/base/thread_win.cpp)
  target_link_libraries(clanker_core PUBLIC psapi bcrypt)
else()
  find_package(Threads REQUIRED)
  target_sources(clanker_core PRIVATE src/base/thread_posix.cpp)
  target_link_libraries(clanker_core PUBLIC Threads::Threads m)
endif()

if(CLANKER_BUILD_CLI)
  include(cmake/curl.cmake)
  include(cmake/pcre2.cmake)
  # Compile CLI implementation once, shared by the executable and its tests.
  add_library(clanker_cli OBJECT
    src/net/http_curl.cpp
    src/net/loopback.cpp
    src/term/term.cpp
    src/term/keys.cpp
    src/term/cell.cpp
    src/term/screen.cpp
    src/term/sanitize.cpp
    src/ui/lineedit.cpp
    src/ui/ui.cpp
    src/ui/layout.cpp
    src/ui/canvas.cpp
    src/ui/event.cpp
    src/ui/widgets.cpp
    src/tui/markdown.cpp
    src/tui/modal.cpp
    src/tui/perf.cpp
    src/tui/selection.cpp
    src/tui/session.cpp
    src/tui/status.cpp
    src/tui/stream_reveal.cpp
    src/tui/linemodel.cpp
    src/tui/toolview.cpp
    src/tui/tui.cpp
    src/llm/responses_proto.cpp
    src/llm/responses.cpp
    src/auth/store.cpp
    src/auth/resolve.cpp
    src/auth/openrouter_oauth.cpp
    src/tools/exec.cpp
    src/tools/search_regex.cpp
    src/tools/tool_select.cpp
    src/tools/truncate.cpp
    src/tools/diff.cpp
    src/tools/filelock.cpp
    src/tools/read.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/ls.cpp
    src/tools/find.cpp
    src/tools/grep.cpp
    src/tools/ask_user.cpp
    src/tools/shell.cpp
    src/app/auth_flow.cpp
    src/app/browser.cpp
    src/app/update.cpp
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
  clanker_target(clanker_cli)
  target_compile_definitions(clanker_cli PUBLIC CLANKER_VERSION="${PROJECT_VERSION}")
  target_link_libraries(clanker_cli PUBLIC clanker_core CURL::libcurl ${CLANKER_PCRE2_TARGET})
  if(WIN32)
    target_sources(clanker_cli PRIVATE src/term/term_win.cpp)
    target_link_libraries(clanker_cli PUBLIC ws2_32 crypt32 advapi32 shell32)
  else()
    target_sources(clanker_cli PRIVATE src/term/term_posix.cpp)
  endif()
  add_executable(clanker src/app/main.cpp)
  clanker_target(clanker)
  target_link_libraries(clanker PRIVATE clanker_cli)
  set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT clanker)
  set_target_properties(clanker PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  install(TARGETS clanker RUNTIME DESTINATION bin COMPONENT runtime)
  install(FILES "${curl_SOURCE_DIR}/COPYING"
    DESTINATION share/clanker/licenses/curl COMPONENT runtime)
  install(FILES "${pcre2_SOURCE_DIR}/LICENCE.md"
    DESTINATION share/clanker/licenses/pcre2 COMPONENT runtime)
endif()

if(CLANKER_BUILD_TESTS)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS tests/suites.txt)
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/tests/suites.txt" suites REGEX "^[^#]")
  set(test_sources tests/test_main.cpp tests/benchmark_agent.cpp)
  set(registration "")
  foreach(row IN LISTS suites)
    string(REPLACE "|" ";" fields "${row}")
    list(LENGTH fields count)
    if(NOT count EQUAL 6)
      message(FATAL_ERROR "Invalid test suite row: ${row}")
    endif()
    list(GET fields 0 group)
    if(NOT group MATCHES "^(core|cli)$")
      message(FATAL_ERROR "Invalid test suite group: ${group}")
    endif()
    if(group STREQUAL "cli" AND NOT CLANKER_BUILD_CLI)
      continue()
    endif()
    list(GET fields 1 name)
    list(GET fields 2 function)
    list(GET fields 3 source)
    list(GET fields 4 tag)
    list(GET fields 5 description)
    string(REPLACE "\\" "\\\\" description "${description}")
    string(REPLACE "\"" "\\\"" description "${description}")
    string(APPEND registration
      "CLANKER_TEST_SUITE(\"${name}\", ${function}, ${tag}, \"${description}\")\n")
    list(APPEND test_sources "tests/${source}.cpp")
  endforeach()
  list(REMOVE_DUPLICATES test_sources)
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/test_suites.inc"
    CONTENT "${registration}")
  add_executable(clanker_tests ${test_sources})
  clanker_target(clanker_tests)
  target_include_directories(clanker_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  if(CLANKER_BUILD_CLI)
    target_link_libraries(clanker_tests PRIVATE clanker_cli)
  else()
    target_link_libraries(clanker_tests PRIVATE clanker_core)
  endif()
  enable_testing()
  add_test(NAME offline COMMAND clanker_tests)
  configure_file(tests/test_platform.cmake generated/test_platform.cmake @ONLY)
  add_test(NAME platform_policy COMMAND "${CMAKE_COMMAND}"
    -P "${CMAKE_CURRENT_BINARY_DIR}/generated/test_platform.cmake")
endif()

if(CLANKER_BUILD_EXAMPLES)
  add_executable(clanker_core_example examples/core/main.cpp)
  clanker_target(clanker_core_example)
  target_link_libraries(clanker_core_example PRIVATE clanker_core)
  set_target_properties(clanker_core_example PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/examples/core/$<0:>")
endif()

install(TARGETS clanker_core ARCHIVE DESTINATION lib COMPONENT sdk)
install(DIRECTORY src/base src/json src/crypto src/session src/agent
  DESTINATION include/clanker COMPONENT sdk FILES_MATCHING PATTERN "*.h")
install(FILES src/llm/types.h src/llm/retry.h DESTINATION include/clanker/llm COMPONENT sdk)
install(FILES src/tools/fs.h src/tools/glob.h DESTINATION include/clanker/tools COMPONENT sdk)
if(WIN32)
  set(linking "Link lib/$<TARGET_FILE_NAME:clanker_core>, psapi.lib and bcrypt.lib.\nCompile with /std:c++latest for MSVC's C++23 support.\nUse /MT for Release, RelWithDebInfo and MinSizeRel; /MDd for Debug.")
else()
  set(linking "Link lib/$<TARGET_FILE_NAME:clanker_core> with -pthread -lm.\nCompile C++23 sources with c++ -std=c++23, then link objects and the archive with cc.")
endif()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/LINKING-$<CONFIG>.txt" CONTENT
  "Clanker core SDK ($<CONFIG>, ${CMAKE_SYSTEM_NAME})\nUse include/clanker as your include directory and a matching C++23 toolchain.\n${linking}\nNo C++ standard library or networking libraries are required by the SDK.\nAll text ownership follows the arena API.\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/LINKING-$<CONFIG>.txt" DESTINATION . COMPONENT sdk)

set(notice_components sdk)
if(CLANKER_BUILD_CLI)
  list(APPEND notice_components runtime)
endif()
foreach(component IN LISTS notice_components)
  install(FILES LICENSE DESTINATION share/clanker/licenses/clanker COMPONENT ${component})
  install(FILES THIRD_PARTY_NOTICES.md DESTINATION share/clanker COMPONENT ${component})
  install(DIRECTORY licenses/ DESTINATION share/clanker/licenses COMPONENT ${component}
    FILES_MATCHING PATTERN LICENSE)
endforeach()
