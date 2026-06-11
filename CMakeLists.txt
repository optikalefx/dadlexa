cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "$ENV{ADF_PATH}/components/esp-sr"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(jacob_smart_speaker)
