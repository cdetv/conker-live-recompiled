# Makes <exe dir>/game a junction to the extracted game folder, unless a
# "game" is already there (CMakeLists.txt, CONKER_GAME_DIR).
# cmake -DEXE_DIR=<dir> -DGAME_DIR=<dir> -P link_game_dir.cmake
if(EXISTS "${EXE_DIR}/game")
    return()
endif()
file(TO_NATIVE_PATH "${EXE_DIR}/game" _link)
file(TO_NATIVE_PATH "${GAME_DIR}" _target)
execute_process(COMMAND cmd /c mklink /J "${_link}" "${_target}"
                RESULT_VARIABLE _rc OUTPUT_QUIET)
if(NOT _rc EQUAL 0)
    message(WARNING "could not link ${_link} to ${_target}")
endif()
