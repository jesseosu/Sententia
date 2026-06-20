# Runs the replay driver twice over the same input and fails if the two
# outputs differ by so much as a byte.
execute_process(COMMAND ${REPLAY} ${INPUT}
                OUTPUT_VARIABLE first
                RESULT_VARIABLE rc1)
if(NOT rc1 EQUAL 0)
    message(FATAL_ERROR "replay failed on first run: ${rc1}")
endif()

execute_process(COMMAND ${REPLAY} ${INPUT}
                OUTPUT_VARIABLE second
                RESULT_VARIABLE rc2)
if(NOT rc2 EQUAL 0)
    message(FATAL_ERROR "replay failed on second run: ${rc2}")
endif()

if(NOT first STREQUAL second)
    message(FATAL_ERROR "replay output differed between runs\n--- first ---\n${first}\n--- second ---\n${second}")
endif()

if(NOT first MATCHES "state_checksum=")
    message(FATAL_ERROR "replay output missing state checksum")
endif()

message(STATUS "replay output identical across runs")
