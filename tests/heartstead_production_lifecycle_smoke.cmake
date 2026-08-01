if(NOT DEFINED HEARTSTEAD OR NOT DEFINED DATA_ROOT)
  message(FATAL_ERROR "HEARTSTEAD and DATA_ROOT are required")
endif()

set(SAVE_ROOT "${DATA_ROOT}/saves")
file(REMOVE_RECURSE "${DATA_ROOT}")

function(run_heartstead output_name)
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" -E env "HEARTSTEAD_DATA_ROOT=${DATA_ROOT}"
      "${HEARTSTEAD}" ${ARGN} --frames 10000
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "heartstead failed (${result}):\n${output}\n${error}")
  endif()
  if(NOT output MATCHES "state=InGame")
    message(FATAL_ERROR "heartstead did not activate the requested world:\n${output}")
  endif()
  if(NOT output MATCHES "autosaves=5")
    message(FATAL_ERROR "bounded production run did not perform five autosaves:\n${output}")
  endif()
  set(${output_name} "${output}" PARENT_SCOPE)
endfunction()

function(read_active_generation slot_path number_name)
  file(READ "${slot_path}/current.txt" manifest)
  string(REGEX MATCH "active\\|generation_([0-9]+)" active_line "${manifest}")
  if(NOT active_line)
    message(FATAL_ERROR "save generation manifest has no active generation: ${manifest}")
  endif()
  set(generation_number "${CMAKE_MATCH_1}")
  if(NOT IS_DIRECTORY "${slot_path}/generations/generation_${generation_number}")
    message(FATAL_ERROR "active save generation does not exist: generation_${generation_number}")
  endif()
  set(${number_name} "${generation_number}" PARENT_SCOPE)
endfunction()

function(read_slot_timestamps slot_path created_name saved_name)
  file(READ "${slot_path}/slot.txt" metadata)
  string(REGEX MATCH "created_at_ms\\|([0-9]+)" created_line "${metadata}")
  if(NOT created_line)
    message(FATAL_ERROR "save slot has no creation timestamp: ${metadata}")
  endif()
  set(created_at "${CMAKE_MATCH_1}")
  string(REGEX MATCH "last_saved_at_ms\\|([0-9]+)" saved_line "${metadata}")
  if(NOT saved_line)
    message(FATAL_ERROR "save slot has no last-saved timestamp: ${metadata}")
  endif()
  set(last_saved_at "${CMAKE_MATCH_1}")
  if(created_at LESS 1500000000000 OR last_saved_at LESS created_at)
    message(FATAL_ERROR
      "save timestamps are not valid Unix wall-clock milliseconds: ${created_at}, ${last_saved_at}")
  endif()
  set(${created_name} "${created_at}" PARENT_SCOPE)
  set(${saved_name} "${last_saved_at}" PARENT_SCOPE)
endfunction()

function(find_slot display_name path_name id_name)
  file(GLOB catalog_entries LIST_DIRECTORIES true "${SAVE_ROOT}/*")
  foreach(entry IN LISTS catalog_entries)
    if(IS_DIRECTORY "${entry}" AND EXISTS "${entry}/slot.txt")
      file(READ "${entry}/slot.txt" metadata)
      string(FIND "${metadata}" "display_name|${display_name}\n" name_position)
      if(NOT name_position EQUAL -1)
        get_filename_component(slot_id "${entry}" NAME)
        set(${path_name} "${entry}" PARENT_SCOPE)
        set(${id_name} "${slot_id}" PARENT_SCOPE)
        return()
      endif()
    endif()
  endforeach()
  message(FATAL_ERROR "save slot '${display_name}' was not found under ${SAVE_ROOT}")
endfunction()

run_heartstead(first_output --new-world "Lifecycle Alpha" --seed 123)
find_slot("Lifecycle Alpha" first_slot first_slot_id)
read_active_generation("${first_slot}" first_generation)
read_slot_timestamps("${first_slot}" first_created first_saved)

# A new persistent world commits its initial snapshot, five periodic autosaves, and one distinct
# final shutdown save. This catches regressions where shutdown loses or merely aliases the last
# periodic snapshot.
if(NOT first_generation EQUAL 7)
  message(FATAL_ERROR
    "expected initial + five autosave + final-save generations, got ${first_generation}")
endif()

run_heartstead(second_output --new-world "Lifecycle Beta" --seed 456)
find_slot("Lifecycle Beta" second_slot second_slot_id)
read_active_generation("${second_slot}" second_generation)
read_slot_timestamps("${second_slot}" second_created second_saved)
if(NOT second_generation EQUAL 7)
  message(FATAL_ERROR "second world did not complete its save lifecycle: ${second_generation}")
endif()

# Start a fresh process and load the first world by catalog id. Its monotonic runtime clock starts
# over, while its persisted wall-clock metadata must remain valid and its transactional generation
# sequence must continue.
run_heartstead(reload_output --world "${first_slot_id}")
read_active_generation("${first_slot}" reloaded_generation)
read_slot_timestamps("${first_slot}" reloaded_created reloaded_saved)
if(NOT reloaded_created EQUAL first_created)
  message(FATAL_ERROR "reloading changed the world's creation timestamp")
endif()
if(reloaded_saved LESS first_saved)
  message(FATAL_ERROR "reloading moved the last-saved wall clock backwards")
endif()
math(EXPR expected_reloaded_generation "${first_generation} + 6")
if(NOT reloaded_generation EQUAL expected_reloaded_generation)
  message(FATAL_ERROR
    "restart did not add five autosaves and a final save: expected ${expected_reloaded_generation}, got ${reloaded_generation}")
endif()

file(GLOB_RECURSE staged_files "${SAVE_ROOT}/*.tmp")
if(staged_files)
  message(FATAL_ERROR "production lifecycle left staged save files behind: ${staged_files}")
endif()

file(REMOVE_RECURSE "${DATA_ROOT}")
