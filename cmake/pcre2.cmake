include(FetchContent)
set(CLANKER_PCRE2_ARCHIVE "" CACHE FILEPATH "Offline copy of the pinned PCRE2 release")
set(pcre2_version 10.48)
set(pcre2_hash ebcc25aadf2a51fa1fefa9b8bc9e7a79b3dae86870a0f1152a22e42befd46888)

set(pcre2_url "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-${pcre2_version}/pcre2-${pcre2_version}.tar.gz")
if(CLANKER_PCRE2_ARCHIVE)
  if(NOT EXISTS "${CLANKER_PCRE2_ARCHIVE}")
    message(FATAL_ERROR "PCRE2 archive does not exist: ${CLANKER_PCRE2_ARCHIVE}")
  endif()
  # FetchContent does not recheck an archive once its sources are populated.
  file(SHA256 "${CLANKER_PCRE2_ARCHIVE}" archive_hash)
  if(NOT archive_hash STREQUAL pcre2_hash)
    message(FATAL_ERROR "PCRE2 archive SHA256 mismatch: ${CLANKER_PCRE2_ARCHIVE}")
  endif()
  set(pcre2_url "${CLANKER_PCRE2_ARCHIVE}")
endif()

function(clanker_fetch_pcre2)
  foreach(option
      BUILD_SHARED_LIBS PCRE2_BUILD_PCRE2_16 PCRE2_BUILD_PCRE2_32
      PCRE2_BUILD_PCRE2GREP PCRE2_BUILD_TESTS PCRE2_SUPPORT_JIT
      PCRE2_SUPPORT_JIT_SEALLOC PCRE2GREP_SUPPORT_JIT PCRE2GREP_SUPPORT_CALLOUT
      PCRE2GREP_SUPPORT_CALLOUT_FORK PCRE2_SUPPORT_LIBBZ2 PCRE2_SUPPORT_LIBZ
      PCRE2_SUPPORT_LIBEDIT PCRE2_SUPPORT_LIBREADLINE PCRE2_SUPPORT_VALGRIND
      PCRE2_REBUILD_CHARTABLES PCRE2_EBCDIC PCRE2_EBCDIC_NL25
      PCRE2_EBCDIC_IGNORING_COMPILER PCRE2_SHOW_REPORT INSTALL_MSVC_PDB)
    set(${option} OFF CACHE BOOL "" FORCE)
  endforeach()
  foreach(option BUILD_STATIC_LIBS PCRE2_BUILD_PCRE2_8 PCRE2_SUPPORT_UNICODE PCRE2_NEVER_BACKSLASH_C)
    set(${option} ON CACHE BOOL "" FORCE)
  endforeach()
  set(PCRE2_NEWLINE LF CACHE STRING "" FORCE)
  set(PCRE2_LINK_SIZE 2 CACHE STRING "" FORCE)

  # Upstream probes these optional tool dependencies even when its tools are off.
  foreach(package BZip2 ZLIB Readline Editline)
    set(CMAKE_DISABLE_FIND_PACKAGE_${package} TRUE)
  endforeach()
  set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
  set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps")
  set(FETCHCONTENT_SOURCE_DIR_PCRE2 "")
  FetchContent_Declare(pcre2
    URL "${pcre2_url}" URL_HASH "SHA256=${pcre2_hash}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR "${FETCHCONTENT_BASE_DIR}/pcre2-src"
    BINARY_DIR "${FETCHCONTENT_BASE_DIR}/pcre2-build"
    SUBBUILD_DIR "${FETCHCONTENT_BASE_DIR}/pcre2-subbuild"
    SOURCE_SUBDIR clanker-populate-only)
  FetchContent_MakeAvailable(pcre2)

  # PCRE2 has no install-disable option. Populate without adding it, then exclude
  # its unconditional install rules and unused POSIX wrapper from the parent.
  # This also works with CMake 3.24, before FetchContent's EXCLUDE_FROM_ALL option.
  add_subdirectory("${pcre2_SOURCE_DIR}" "${pcre2_BINARY_DIR}" EXCLUDE_FROM_ALL)
  # Do not force a CRT switch: upstream inherits CMAKE_MSVC_RUNTIME_LIBRARY.
  set(pcre2_SOURCE_DIR "${pcre2_SOURCE_DIR}" PARENT_SCOPE)
  set(pcre2_BINARY_DIR "${pcre2_BINARY_DIR}" PARENT_SCOPE)
endfunction()

clanker_fetch_pcre2()
set(CLANKER_PCRE2_TARGET pcre2-8-static)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/clanker-pcre2.txt"
  CONTENT "${pcre2_SOURCE_DIR}\n${pcre2_version}\n${pcre2_hash}\n")
