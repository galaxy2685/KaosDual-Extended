cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kaos_esp32_dual)

# Stage the PC-side library below a top-level skylandersDumps directory, so
# the exact same hierarchy is available at /spiffs/skylandersDumps at runtime.
set(KAOS_LIBRARY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/skylandersDumps")
set(KAOS_SPIFFS_STAGE "${CMAKE_BINARY_DIR}/kaos_spiffs_data")
if(EXISTS "${KAOS_LIBRARY_SOURCE}")
    # This is a generated build-directory staging area. Recreate it so removed
    # source dumps cannot remain in a subsequent SPIFFS image.
    file(REMOVE_RECURSE "${KAOS_SPIFFS_STAGE}")
    file(MAKE_DIRECTORY "${KAOS_SPIFFS_STAGE}")
    # The dump collection also contains future web-image assets. Package only
    # raw .sky files so images and every other non-dump asset stay off SPIFFS.
    file(GLOB_RECURSE KAOS_SKY_FILES CONFIGURE_DEPENDS
         LIST_DIRECTORIES false "${KAOS_LIBRARY_SOURCE}/*.sky")
    foreach(KAOS_SKY_FILE IN LISTS KAOS_SKY_FILES)
        file(RELATIVE_PATH KAOS_SKY_REL "${KAOS_LIBRARY_SOURCE}" "${KAOS_SKY_FILE}")
        get_filename_component(KAOS_SKY_DIR "${KAOS_SKY_REL}" DIRECTORY)
        file(MAKE_DIRECTORY "${KAOS_SPIFFS_STAGE}/skylandersDumps/${KAOS_SKY_DIR}")
        configure_file("${KAOS_SKY_FILE}"
                       "${KAOS_SPIFFS_STAGE}/skylandersDumps/${KAOS_SKY_REL}" COPYONLY)
    endforeach()
    spiffs_create_partition_image(spiffs "${KAOS_SPIFFS_STAGE}" FLASH_IN_PROJECT)
else()
    message(WARNING "No esp32/skylandersDumps directory: SPIFFS library image will be empty")
endif()
