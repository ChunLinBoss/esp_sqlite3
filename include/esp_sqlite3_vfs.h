/**
 * @file esp_sqlite3_vfs.h
 * @brief esp_sqlite3 自定义 VFS 的注册、统计与路径工具
 *
 * 该 VFS 把 SQLite 的文件操作映射到 ESP-IDF VFS 的 POSIX 接口，
 * 因此 SPIFFS / FATFS / LittleFS / SD 卡等只要挂载后能用 open()/read()/
 * write()/ftruncate()/fsync() 访问，就可以直接存放 SQLite 数据库。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "sqlite3.h"

#ifdef __cplusplus
extern "C" {
#endif

/** VFS 运行统计（用于性能分析和问题定位） */
typedef struct {
    uint32_t open_files;        /**< 当前打开的 SQLite 文件数 */
    uint32_t total_opens;       /**< 累计打开次数 */
    uint32_t read_ops;          /**< 累计 read 调用次数 */
    uint32_t write_ops;         /**< 累计 write 调用次数 */
    uint32_t sync_ops;          /**< 累计 fsync 次数 */
    uint32_t truncate_ops;      /**< 累计 ftruncate 次数 */
    uint32_t lock_busy;         /**< 因锁冲突返回 SQLITE_BUSY 的次数 */
    uint64_t bytes_read;        /**< 累计读取字节数 */
    uint64_t bytes_written;     /**< 累计写入字节数 */
} esp_sqlite3_vfs_stats_t;

/**
 * @brief 获取本组件的 sqlite3_vfs 指针
 *
 * 即使尚未注册也可以安全调用，用于查看名称/版本/最大路径等。
 */
sqlite3_vfs *esp_sqlite3_vfs_get(void);

/**
 * @brief 手动注册 VFS
 *
 * 正常情况下 sqlite3_os_init() 会在 sqlite3_initialize() 时自动注册，
 * 只有需要自定义注册顺序或指定非默认 VFS 时才需要手动调用。
 *
 * @param make_default true = 注册为默认 VFS
 */
esp_err_t esp_sqlite3_vfs_register(bool make_default);

/** @brief 读取统计（线程安全快照） */
void esp_sqlite3_vfs_get_stats(esp_sqlite3_vfs_stats_t *out);

/** @brief 清零统计 */
void esp_sqlite3_vfs_reset_stats(void);

/**
 * @brief 把相对路径补全为绝对路径
 *
 * 规则：
 *  - 以 '/' 开头：原样复制；
 *  - ":memory:"：原样复制；
 *  - 其他：拼到 Kconfig 的 ESP_SQLITE3_DEFAULT_DIR 后面。
 *
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE
 */
esp_err_t esp_sqlite3_vfs_make_path(const char *path, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
