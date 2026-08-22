cmake_minimum_required(VERSION 3.16)

# The otool_llm_sdk component lives two levels above this test app.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(otool_llm_sdk_transport_test)
