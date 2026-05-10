cmake_minimum_required(VERSION 3.16)

# Pull in libscadable as a local override so the example builds against the
# in-repo source. Real customer projects fetch via idf_component.yml.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-idf-captive-provisioning)
