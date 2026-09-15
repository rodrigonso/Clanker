foreach(cli ON OFF)
  set(build "@CMAKE_CURRENT_BINARY_DIR@/platform-policy/cli-${cli}")
  execute_process(
    COMMAND "@CMAKE_COMMAND@" -S "@CMAKE_CURRENT_SOURCE_DIR@" -B "${build}"
      -G "@CMAKE_GENERATOR@" "-DCMAKE_MAKE_PROGRAM:FILEPATH=@CMAKE_MAKE_PROGRAM@"
      -DCMAKE_SYSTEM_NAME=Linux "-DCLANKER_BUILD_CLI=${cli}"
      -DCMAKE_C_COMPILER=clanker-unsupported-compiler
      -DCMAKE_CXX_COMPILER=clanker-unsupported-compiler
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(result STREQUAL "0" OR NOT "${output}${error}" MATCHES "Clanker supports only Windows and macOS")
    message(FATAL_ERROR "Unsupported target was not rejected before compiler setup (CLI=${cli}):\n${output}${error}")
  endif()
  if(EXISTS "${build}/_deps")
    message(FATAL_ERROR "Unsupported target populated dependencies (CLI=${cli}).")
  endif()
endforeach()
message(STATUS "Unsupported CLI and core targets rejected before compiler setup.")
