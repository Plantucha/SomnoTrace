# SomnoTrace - top-level ESP-IDF project file
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS ${EXTRA_COMPONENT_DIRS} "third_party/esp-idf-ftpServer" "third_party/libsmb2" "components/uploader")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(somnotrace)
