# esp_sqlite3 自测工程
#
# 独立于主固件，使用 4MB Flash 的 factory + SPIFFS 分区，
# 方便在开发板上单独验证 VFS / WAL / FTS5 / RTREE / shox96 等功能。
cmake_minimum_required(VERSION 3.22)

# 指向 components 目录，让本工程能找到 esp_sqlite3 组件
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_sqlite3_selftest)
