/**
 * @file esp_sqlite3_internal.h
 * @brief esp_sqlite3 组件内部接口（不对外暴露）
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "sqlite3.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 组件日志 TAG */
#define ESP_SQLITE3_TAG "esp_sqlite3"

#define ESP_SQLITE3_LOGE(fmt, ...) ESP_LOGE(ESP_SQLITE3_TAG, fmt, ##__VA_ARGS__)
#define ESP_SQLITE3_LOGW(fmt, ...) ESP_LOGW(ESP_SQLITE3_TAG, fmt, ##__VA_ARGS__)
#define ESP_SQLITE3_LOGI(fmt, ...) ESP_LOGI(ESP_SQLITE3_TAG, fmt, ##__VA_ARGS__)
#define ESP_SQLITE3_LOGD(fmt, ...) ESP_LOGD(ESP_SQLITE3_TAG, fmt, ##__VA_ARGS__)

/** 默认 VFS 名称（Kconfig 可改） */
#ifndef CONFIG_ESP_SQLITE3_VFS_NAME
#define CONFIG_ESP_SQLITE3_VFS_NAME "esp_sqlite3"
#endif

/** 最大路径长度 */
#ifndef CONFIG_ESP_SQLITE3_MAX_PATH
#define CONFIG_ESP_SQLITE3_MAX_PATH 128
#endif

/** 默认目录 / 临时目录兜底值 */
#ifndef CONFIG_ESP_SQLITE3_DEFAULT_DIR
#define CONFIG_ESP_SQLITE3_DEFAULT_DIR "/db"
#endif
#ifndef CONFIG_ESP_SQLITE3_TEMP_DIR
#define CONFIG_ESP_SQLITE3_TEMP_DIR "/db"
#endif

/* -------------------------------------------------------------------------
 * 内存分配器（esp_sqlite3_mem.c）
 * ------------------------------------------------------------------------- */

/** 注册 SQLite 内存方法；必须在 sqlite3_initialize() 之前调用 */
esp_err_t esp_sqlite3_mem_register(void);

/** 当前分配器是否优先使用 PSRAM */
bool esp_sqlite3_mem_uses_psram(void);

/* -------------------------------------------------------------------------
 * 互斥量（esp_sqlite3_mutex.c）
 * ------------------------------------------------------------------------- */

/** 注册 FreeRTOS 递归互斥量方法；必须在 sqlite3_initialize() 之前调用 */
esp_err_t esp_sqlite3_mutex_register(void);

/* -------------------------------------------------------------------------
 * 扩展函数（esp_sqlite3_ext.c）
 * ------------------------------------------------------------------------- */

/** 注册 shox96 与 esp_sqlite_* 扩展函数（作为 auto extension） */
esp_err_t esp_sqlite3_ext_register(void);

/** 注销扩展函数 */
void esp_sqlite3_ext_unregister(void);

/** shox96 扩展的 SQL 函数入口（sqlite3_auto_extension 需要） */
int esp_sqlite3_shox96_init(sqlite3 *db, char **pzErrMsg,
                           const sqlite3_api_routines *pThunk);

/* -------------------------------------------------------------------------
 * 通用工具
 * ------------------------------------------------------------------------- */

/** 把 SQLite 结果码 + 上下文写入日志（带扩展错误码与偏移） */
void esp_sqlite3_log_error(sqlite3 *db, int rc, const char *what);

/** 内部：确保运行时已初始化 */
esp_err_t esp_sqlite3_ensure_init(void);

#ifdef __cplusplus
}
#endif
