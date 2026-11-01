cmake_minimum_required(VERSION 3.22)

# 指向 components 目录，让本工程能找到 esp_sqlite3 组件
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_sqlite3_stress)
