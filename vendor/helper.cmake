function(find_reachable_git_url OUT_REACHABLE_URL IN_URL_LIST)
    find_program(GIT_EXECUTABLE git)
    if(NOT GIT_EXECUTABLE)
        message(FATAL_ERROR "git not found!")
    endif()

    set(GIT_PROBE_TIMEOUT 30)
    if(DEFINED DEP_GIT_PROBE_TIMEOUT)
        set(GIT_PROBE_TIMEOUT ${DEP_GIT_PROBE_TIMEOUT})
    endif()

    if(DEFINED ${IN_URL_LIST})
        set(URL_LIST ${${IN_URL_LIST}})
    else()
        set(URL_LIST ${ARGN})
    endif()

    foreach(GIT_URL IN LISTS URL_LIST)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} ls-remote --heads "${GIT_URL}"
            RESULT_VARIABLE GIT_RESULT
            OUTPUT_QUIET ERROR_QUIET
            TIMEOUT ${GIT_PROBE_TIMEOUT}
        )
        if(GIT_RESULT EQUAL 0)
            set(${OUT_REACHABLE_URL} ${GIT_URL} PARENT_SCOPE)
            return()
        endif()
    endforeach()

    set(${OUT_REACHABLE_URL} "" PARENT_SCOPE)
endfunction()
