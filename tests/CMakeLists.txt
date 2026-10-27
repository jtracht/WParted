# SPDX-License-Identifier: GPL-2.0-or-later
add_executable(wparted_core_tests unit/test_core.cpp)
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
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/cli_image_roundtrip.cmake)
endif()
