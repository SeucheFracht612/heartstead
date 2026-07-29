if(NOT DEFINED DEV_GAME OR NOT DEFINED SAVE_ROOT)
  message(FATAL_ERROR "DEV_GAME and SAVE_ROOT are required")
endif()

file(REMOVE_RECURSE "${SAVE_ROOT}")

function(read_active_generation output_name)
  file(READ "${SAVE_ROOT}/current.txt" manifest)
  string(REGEX MATCH "active\\|(generation_[0-9]+)" active_line "${manifest}")
  if(NOT active_line)
    message(FATAL_ERROR "save generation manifest has no active generation: ${manifest}")
  endif()
  set(${output_name} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

function(run_dev_game expected_save_text)
  execute_process(
    COMMAND "${DEV_GAME}" --frames 65 --save "${SAVE_ROOT}" --autosave-seconds 1
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "dev_game failed (${result}): ${error}")
  endif()
  if(NOT output MATCHES "save=${expected_save_text}")
    message(FATAL_ERROR "unexpected dev_game summary: ${output}")
  endif()
  if(NOT output MATCHES "autosaves=1")
    message(FATAL_ERROR "periodic save did not run exactly once: ${output}")
  endif()
endfunction()

run_dev_game("written")

if(NOT EXISTS "${SAVE_ROOT}/current.txt")
  message(FATAL_ERROR "new save did not commit a generation manifest")
endif()
read_active_generation(first_generation)
if(NOT IS_DIRECTORY "${SAVE_ROOT}/generations/${first_generation}")
  message(FATAL_ERROR "new save CURRENT generation is invalid")
endif()

run_dev_game("loaded\\+written")

read_active_generation(second_generation)
if("${second_generation}" STREQUAL "${first_generation}" OR
   NOT IS_DIRECTORY "${SAVE_ROOT}/generations/${second_generation}")
  message(FATAL_ERROR "clean shutdown did not transactionally commit a new generation")
endif()

file(GLOB staged_generations "${SAVE_ROOT}/generations/*.tmp")
if(staged_generations)
  message(FATAL_ERROR "save left staged generations behind: ${staged_generations}")
endif()

file(REMOVE_RECURSE "${SAVE_ROOT}")
