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

# GOLDEN VALUES.
#
# Comparing a run against itself only proves the engine is repeatable. It
# says nothing about whether the answer is RIGHT: change the matching
# rules and both runs change together, agreeing perfectly on new and
# wrong output.
#
# That is the same "agreement is not correctness" trap the replication
# tests had. These three numbers were quoted for five phases as evidence
# that nothing had regressed, while being asserted nowhere and compared
# by eye. Now they are pinned.
#
# If a change to matching semantics is INTENDED, these values change and
# the new ones go here deliberately. That is the point: it becomes a
# decision instead of a silent drift.
foreach(pair "event_hash=${EXPECT_EVENT_HASH}"
             "book_checksum=${EXPECT_BOOK_CHECKSUM}"
             "state_checksum=${EXPECT_STATE_CHECKSUM}")
    if(NOT first MATCHES "${pair}")
        message(FATAL_ERROR
            "replay produced different output than the pinned golden value.\n"
            "expected to find: ${pair}\n"
            "--- actual ---\n${first}\n"
            "If this change was intended, update the EXPECT_ values in CMakeLists.txt.")
    endif()
endforeach()

message(STATUS "replay output identical across runs and matches the golden checksums")
