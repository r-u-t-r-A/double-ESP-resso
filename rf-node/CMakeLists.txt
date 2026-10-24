cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rf-node)

# The PHY dump engine owns SRAM 0x40820000-0x4083FFFF while it captures
# (main/iq_dump.c). Fail the build if code or static data reaches into it,
# or if the RF test library symbols the capture needs are missing.
idf_build_get_property(python PYTHON)
add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
    COMMAND ${python} ${CMAKE_CURRENT_LIST_DIR}/check_sram.py ${CMAKE_NM}
            $<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf>
    VERBATIM)
