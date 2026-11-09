# Runs the recompiled and interpreted headless builds over the same scripted
# session and requires identical per-frame state digests.
foreach(side RECOMP INTERP)
  execute_process(COMMAND "${${side}}" "${ROM}" "${FRAMES}" ${MODE}
                  OUTPUT_FILE "${OUT}_${side}.txt" ERROR_VARIABLE log RESULT_VARIABLE rc)
  message(STATUS "${side}: ${log}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${side} run failed (${rc})")
  endif()
endforeach()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}_RECOMP.txt" "${OUT}_INTERP.txt"
                RESULT_VARIABLE diff)
if(NOT diff EQUAL 0)
  message(FATAL_ERROR "recompiled state diverged from the interpreter; see ${OUT}_*.txt")
endif()
message(STATUS "all ${FRAMES} frames identical (${MODE})")
