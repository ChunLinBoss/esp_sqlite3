/**
 * @file esp_sqlite3_force_ndebug.h
 * @brief 强制 SQLite 的 assert() 在 NDEBUG 下完全不求值
 *
 * ESP-IDF 5.x 的 assert.h 有一个选项
 * CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE：定义 NDEBUG 时仍然求值 assert 表达式
 * （实际是 `(void)(expr)`）。SQLite 的 amalgamation 假设“NDEBUG => assert 参数
 * 不被求值”，它会引用只在 SQLITE_DEBUG 下才存在的字段/变量，例如：
 *   assert( GLOBAL(int, mutexIsInit) );
 * 在 IDF 5.5 默认配置下因此编译失败。
 *
 * 本头文件只对 sqlite3.c 使用 -include 强制包含：
 *   1. 先包含 sdkconfig.h（确保宏已定义且 include guard 生效）；
 *   2. 取消 CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE；
 * 之后 assert.h 会走 `#define assert(__e) ((void)0)` 分支。
 */
#pragma once

#include "sdkconfig.h"

#undef CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE
