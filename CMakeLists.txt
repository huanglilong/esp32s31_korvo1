cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s31_korvo1)

# ─────────────────────────────────────────────────────────────────
#  Suppress known GCC warnings from IDF BT Bluedroid stack
#  (stringop-truncation with strncpy of BD_NAME_LEN bytes in GCC 12+)
# ─────────────────────────────────────────────────────────────────
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-truncation" APPEND)

# ─────────────────────────────────────────────────────────────────
#  uORB topic code generation from proto/*.msg
#  Automatically runs on idf.py build when .msg files change.
#  Regenerate manually:  idf.py uorb_topics
# ─────────────────────────────────────────────────────────────────
set(UORB_MSG_DIR  ${CMAKE_SOURCE_DIR}/proto)
set(UORB_GEN_DIR  ${CMAKE_SOURCE_DIR}/main/generated)
set(UORB_GEN_PY   ${CMAKE_SOURCE_DIR}/tools/msg_gen.py)

file(GLOB UORB_MSG_FILES  ${UORB_MSG_DIR}/*.msg)

# List every generated output file explicitly
set(UORB_GEN_FILES
    ${UORB_GEN_DIR}/audio_level.h         ${UORB_GEN_DIR}/audio_level.cpp
    ${UORB_GEN_DIR}/camera_state.h        ${UORB_GEN_DIR}/camera_state.cpp
    ${UORB_GEN_DIR}/camera_frame.h        ${UORB_GEN_DIR}/camera_frame.cpp
    ${UORB_GEN_DIR}/fps_stats.h           ${UORB_GEN_DIR}/fps_stats.cpp
    ${UORB_GEN_DIR}/recording_state.h     ${UORB_GEN_DIR}/recording_state.cpp
    ${UORB_GEN_DIR}/ulog_state.h          ${UORB_GEN_DIR}/ulog_state.cpp
    ${UORB_GEN_DIR}/volume_state.h        ${UORB_GEN_DIR}/volume_state.cpp
    ${UORB_GEN_DIR}/wifi_state.h          ${UORB_GEN_DIR}/wifi_state.cpp
    ${UORB_GEN_DIR}/system_stats.h        ${UORB_GEN_DIR}/system_stats.cpp
    ${UORB_GEN_DIR}/system_alert.h        ${UORB_GEN_DIR}/system_alert.cpp
    ${UORB_GEN_DIR}/uORBTopics.hpp        ${UORB_GEN_DIR}/uORBTopics.cpp
)

# add_custom_command(OUTPUT ...) tells CMake:
# "these files are generated — build them before any target that depends on them"
add_custom_command(OUTPUT ${UORB_GEN_FILES}
    COMMAND python3 ${UORB_GEN_PY}
        --headers --sources --topics
        -f ${UORB_MSG_FILES}
        -o ${UORB_GEN_DIR}
    DEPENDS ${UORB_MSG_FILES} ${UORB_GEN_PY}
    COMMENT "Generating uORB topic files from proto/*.msg"
    VERBATIM
    )

# Custom target for manual regeneration
add_custom_target(uorb_topics DEPENDS ${UORB_GEN_FILES})