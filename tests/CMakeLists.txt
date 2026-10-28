# SPDX-License-Identifier: GPL-2.0-or-later
add_executable(wparted_core_tests unit/test_core.cpp unit/test_operations.cpp unit/test_rescue.cpp unit/test_executor.cpp unit/test_fstools.cpp unit/test_helper_protocol.cpp)
target_link_libraries(wparted_core_tests PRIVATE wparted_core)
add_test(NAME core_unit_tests COMMAND wparted_core_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

if(WIN32 AND TARGET wparted-cli)
    # The CLI must analyse a synthetic image end-to-end and emit valid JSON.
    add_executable(wparted_make_test_image integration/make_test_image.cpp)
    target_link_libraries(wparted_make_test_image PRIVATE wparted_core)
    target_include_directories(wparted_make_test_image PRIVATE unit)
    add_test(NAME cli_image_roundtrip
        COMMAND ${CMAKE_COMMAND}
            -DMAKE_IMAGE=$<TARGET_FILE:wparted_make_test_image>
            -DCLI=$<TARGET_FILE:wparted-cli>
            -DIMAGE_TOOL=$<TARGET_FILE:wparted_image_tool>
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
            "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/cli_image_roundtrip.cmake)
endif()

# Detection probe for images made by the real mkfs tools (tools/ci/mkfs-fixtures.sh, Linux CI).
add_executable(wparted_detect_fs integration/detect_fs.cpp)
target_link_libraries(wparted_detect_fs PRIVATE wparted_core)

# Plan creation/execution driver for image files (tools/ci/image-apply-check.sh, CLI end-to-end test).
add_executable(wparted_image_tool integration/image_tool.cpp)
target_link_libraries(wparted_image_tool PRIVATE wparted_core)
