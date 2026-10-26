/**
 * @file esp_sqlite3.h
 * @brief 面向 ESP-IDF 的 SQLite3 高层封装（中文注释版）
 *
 * 设计目标
 * --------
 * 1. 直接使用 SQLite 官方 amalgamation（当前 3.54.1），不修改 sqlite3.c；
 * 2. 通过自定义 VFS 把 SQLite 的磁盘 IO 接到 ESP-IDF VFS：
 *    SPIFFS / FATFS / LittleFS / SD 卡 / 自定义文件系统均可；
 * 3. 提供真正可用的文件锁、日志文件、fsync、ftruncate、WAL 共享内存，
 *    不再像旧社区版那样“锁是空实现、日志放 RAM、截断是空函数”；
 * 4. 兼容 ESP-IDF v5.x 与 v6.x（v5 以前不保证）；
 * 5. 高层 API 统一用 esp_err_t 返回，并保留底层 sqlite3* 句柄，
 *    需要高级用法时可以直接调用 SQLite 原生 API。
 *
 * 线程模型
 * --------
 * - 默认 SQLITE_THREADSAFE=1（串行化模式），互斥量由 FreeRTOS 递归信号量实现；
 * - 同一个 esp_sqlite3_t 连接可以被多个任务“轮流”使用，但不要并发使用同一个
 *   sqlite3_stmt；需要并发读写时，请为每个任务分别 esp_sqlite3_open()，
 *   VFS 会通过进程内文件锁保证一致性。
 *
 * 典型用法
 * --------
 * @code
 *   esp_sqlite3_init();
 *   esp_sqlite3_t *db = NULL;
 *   esp_sqlite3_config_t cfg;
 *   esp_sqlite3_config_init(&cfg);
 *   cfg.path = "/spiffs/app.db";
 *   ESP_ERROR_CHECK(esp_sqlite3_open(&cfg, &db));
 *   ESP_ERROR_CHECK(esp_sqlite3_exec(db,
 *       "CREATE TABLE IF NOT EXISTS kv(k TEXT PRIMARY KEY, v TEXT);"));
 *   ESP_ERROR_CHECK(esp_sqlite3_exec(db,
 *       "INSERT OR REPLACE INTO kv VALUES('boot', '1');"));
 *   esp_sqlite3_close(db);
 * @endcode
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sqlite3.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 本组件封装的 SQLite 版本号 */
#define ESP_SQLITE3_VERSION_STRING "3.54.1"

/** 不透明数据库句柄 */
typedef struct esp_sqlite3 esp_sqlite3_t;

/** 日志模式，对应 PRAGMA journal_mode */
typedef enum {
    ESP_SQLITE3_JOURNAL_DEFAULT = -1, /**< 保持数据库现有模式（新建库即 DELETE） */
    ESP_SQLITE3_JOURNAL_DELETE = 0,   /**< 回滚日志，提交后删除（掉电安全） */
    ESP_SQLITE3_JOURNAL_TRUNCATE,     /**< 回滚日志，提交后截断为 0 */
    ESP_SQLITE3_JOURNAL_PERSIST,      /**< 回滚日志，提交后保留文件头 */
    ESP_SQLITE3_JOURNAL_MEMORY,       /**< 日志放内存（掉电可能损坏数据库） */
    ESP_SQLITE3_JOURNAL_WAL,          /**< 预写日志（需要编译时开启 WAL） */
    ESP_SQLITE3_JOURNAL_OFF,          /**< 关闭日志（最危险，仅用于只读/可重建数据） */
} esp_sqlite3_journal_mode_t;

/** 同步级别，对应 PRAGMA synchronous */
typedef enum {
    ESP_SQLITE3_SYNC_OFF = 0,     /**< 不等待落盘，最快但掉电可能损坏 */
    ESP_SQLITE3_SYNC_NORMAL = 1,  /**< WAL 模式推荐 */
    ESP_SQLITE3_SYNC_FULL = 2,    /**< 回滚日志模式推荐（默认） */
    ESP_SQLITE3_SYNC_EXTRA = 3,   /**< 最安全，最慢 */
} esp_sqlite3_sync_mode_t;

/**
 * @brief 打开数据库的配置
 *
 * 使用 esp_sqlite3_config_init() 填充默认值，再按需修改。
 * 所有字符串指针只在 esp_sqlite3_open() 期间使用，函数返回后可以释放。
 */
typedef struct {
    /** 数据库路径；":memory:" 表示纯内存库；相对路径会拼到默认目录下 */
    const char *path;

    /** VFS 名称，NULL 表示使用本组件注册的默认 VFS（"esp_sqlite3"） */
    const char *vfs;

    /** true = 只读打开（SQLITE_OPEN_READONLY） */
    bool read_only;

    /** true = 文件不存在时创建（只读模式下忽略） */
    bool create;

    /** true = 允许 file: URI 语法（SQLITE_OPEN_URI） */
    bool uri;

    /** busy 超时（毫秒），0 表示不等待；默认 5000 */
    uint32_t busy_timeout_ms;

    /** 页缓存大小（KiB），0 表示使用编译默认值 */
    uint32_t cache_size_kib;

    /** 页大小（字节），0 表示使用编译默认值；仅对新建数据库生效 */
    uint32_t page_size;

    /** 日志模式 */
    esp_sqlite3_journal_mode_t journal_mode;

    /** 同步级别 */
    esp_sqlite3_sync_mode_t synchronous;

    /** true = 开启外键约束（PRAGMA foreign_keys=ON） */
    bool foreign_keys;

    /** true = 临时表/排序放内存（PRAGMA temp_store=MEMORY） */
    bool temp_store_memory;

    /** true = 独占锁模式（PRAGMA locking_mode=EXCLUSIVE） */
    bool exclusive_locking;

    /** true = 防御模式（SQLITE_DBCONFIG_DEFENSIVE，禁止写系统表/危险 PRAGMA） */
    bool defensive;

    /** true = 只允许查询（PRAGMA query_only=ON） */
    bool query_only;

    /** true = 打开后执行一次 PRAGMA optimize（推荐长期运行设备） */
    bool auto_optimize;
} esp_sqlite3_config_t;

/** 内存统计（来自 sqlite3_status64） */
typedef struct {
    int64_t memory_used;          /**< 当前 SQLite 堆内存（字节） */
    int64_t memory_highwater;     /**< 历史峰值（字节） */
    int64_t pagecache_used;       /**< 页缓存当前使用的槽位数 */
    int64_t pagecache_highwater;  /**< 页缓存槽位峰值 */
    int64_t pagecache_overflow;   /**< 页缓存溢出次数（说明缓存偏小） */
    int64_t malloc_count;         /**< 当前仍存活的分配块数 */
} esp_sqlite3_mem_stats_t;

/** 行回调：返回 ESP_OK 继续，返回其他值会中止查询 */
typedef esp_err_t (*esp_sqlite3_row_cb_t)(void *ctx, sqlite3_stmt *stmt, int row_index);

/** 事务回调：返回 ESP_OK 提交，其他值回滚 */
typedef esp_err_t (*esp_sqlite3_txn_cb_t)(void *ctx);

/** 数据库迁移步骤 */
typedef struct {
    int version;        /**< 执行完 sql 后写入 PRAGMA user_version 的值 */
    const char *sql;    /**< 可包含多条语句，整体在一个事务内执行 */
} esp_sqlite3_migration_t;

/* =========================================================================
 * 生命周期
 * ========================================================================= */

/**
 * @brief 初始化 SQLite 运行时（幂等）
 *
 * 完成以下工作：
 *  - 注册 FreeRTOS 递归互斥量实现（SQLITE_CONFIG_MUTEX）
 *  - 注册 PSRAM 优先的内存分配器（SQLITE_CONFIG_MALLOC）
 *  - 注册 esp_sqlite3 VFS（由 sqlite3_os_init 完成）
 *  - 注册内置扩展函数（shox96 压缩、esp_sqlite_* 辅助函数）
 *
 * @note 必须在任何 sqlite3_* API 之前调用；重复调用安全。
 */
esp_err_t esp_sqlite3_init(void);

/**
 * @brief 关闭 SQLite 运行时
 *
 * 会检查是否还有未关闭的数据库连接；有连接时返回
 * ESP_ERR_INVALID_STATE，并且不会调用 sqlite3_shutdown()。
 * 一般只在测试/重启前使用，正常业务无需调用。
 *
 * @return ESP_OK 或错误码
 */
esp_err_t esp_sqlite3_deinit(void);

/** @brief 是否已经初始化 */
bool esp_sqlite3_is_initialized(void);

/** @brief 返回 SQLite 版本字符串，例如 "3.54.1" */
const char *esp_sqlite3_version(void);

/** @brief 设置本组件的日志级别（ESP_LOG_xxx） */
void esp_sqlite3_set_log_level(int level);

/* =========================================================================
 * 打开 / 关闭
 * ========================================================================= */

/** @brief 用默认值填充配置结构体 */
void esp_sqlite3_config_init(esp_sqlite3_config_t *cfg);

/**
 * @brief 打开数据库
 * @param cfg 配置，不能为 NULL，path 不能为 NULL
 * @param out_db 输出句柄
 */
esp_err_t esp_sqlite3_open(const esp_sqlite3_config_t *cfg, esp_sqlite3_t **out_db);

/** @brief 使用默认配置打开数据库（不存在则创建） */
esp_err_t esp_sqlite3_open_simple(const char *path, esp_sqlite3_t **out_db);

/**
 * @brief 关闭数据库；句柄置空由调用者负责
 *
 * 使用 sqlite3_close() 而不是 close_v2()，因此如果仍有未 finalize 的语句，
 * 会返回 ESP_ERR_INVALID_STATE，连接不会被释放，调用者可先清理语句再重试。
 */
esp_err_t esp_sqlite3_close(esp_sqlite3_t *db);

/** @brief 获取底层 sqlite3*，用于调用原生 API */
sqlite3 *esp_sqlite3_handle(esp_sqlite3_t *db);

/** @brief 获取打开时的路径 */
const char *esp_sqlite3_path(const esp_sqlite3_t *db);

/** @brief 是否处于事务中（sqlite3_get_autocommit() == 0） */
bool esp_sqlite3_in_transaction(const esp_sqlite3_t *db);

/* =========================================================================
 * 执行 SQL
 * ========================================================================= */

/** @brief 执行一条或多条 SQL，忽略结果行 */
esp_err_t esp_sqlite3_exec(esp_sqlite3_t *db, const char *sql);

/** @brief 执行 SQL 并对每一行调用 callback（兼容 sqlite3_exec 风格） */
esp_err_t esp_sqlite3_exec_cb(esp_sqlite3_t *db, const char *sql,
                             int (*callback)(void *, int, char **, char **),
                             void *arg);

/**
 * @brief 执行查询并对每一行调用 cb
 *
 * cb 返回 ESP_OK 继续；返回其他 esp_err_t 会中止查询并原样返回。
 */
esp_err_t esp_sqlite3_query(esp_sqlite3_t *db, const char *sql,
                           esp_sqlite3_row_cb_t cb, void *ctx);

/** @brief 查询单个 int64（第一行第一列），无结果时返回 ESP_ERR_NOT_FOUND */
esp_err_t esp_sqlite3_query_int64(esp_sqlite3_t *db, const char *sql, int64_t *out);

/** @brief 查询单个 double，无结果时返回 ESP_ERR_NOT_FOUND */
esp_err_t esp_sqlite3_query_double(esp_sqlite3_t *db, const char *sql, double *out);

/**
 * @brief 查询单个字符串并复制到 buf
 *
 * @param buf 输出缓冲区；若为 NULL 则只检查是否存在
 * @param buf_len 缓冲区长度（包含结尾 '\0'）
 * @return ESP_ERR_NOT_FOUND 无结果；ESP_ERR_INVALID_SIZE 缓冲区不足
 */
esp_err_t esp_sqlite3_query_text(esp_sqlite3_t *db, const char *sql,
                                char *buf, size_t buf_len);

/* =========================================================================
 * 预编译语句
 * ========================================================================= */

/** @brief 预编译一条 SQL；返回 ESP_OK 时 *stmt 需要 sqlite3_finalize() */
esp_err_t esp_sqlite3_prepare(esp_sqlite3_t *db, const char *sql, sqlite3_stmt **stmt);

/** @brief 预编译（带长度与 tail 指针），语义同 sqlite3_prepare_v2 */
esp_err_t esp_sqlite3_prepare_v2(esp_sqlite3_t *db, const char *sql, int sql_len,
                                sqlite3_stmt **stmt, const char **tail);

/** @brief 单步执行，返回 SQLITE_ROW / SQLITE_DONE / 其他错误码 */
int esp_sqlite3_step(sqlite3_stmt *stmt);

/** @brief 复位语句，保留绑定值 */
esp_err_t esp_sqlite3_reset(sqlite3_stmt *stmt);

/** @brief 清空所有绑定值 */
esp_err_t esp_sqlite3_clear_bindings(sqlite3_stmt *stmt);

/** @brief 释放语句；允许传入 NULL */
esp_err_t esp_sqlite3_finalize(sqlite3_stmt *stmt);

esp_err_t esp_sqlite3_bind_null(sqlite3_stmt *stmt, int index);
esp_err_t esp_sqlite3_bind_int(sqlite3_stmt *stmt, int index, int value);
esp_err_t esp_sqlite3_bind_int64(sqlite3_stmt *stmt, int index, int64_t value);
esp_err_t esp_sqlite3_bind_double(sqlite3_stmt *stmt, int index, double value);
/** @brief 绑定文本；SQLITE_TRANSIENT，会复制一份 */
esp_err_t esp_sqlite3_bind_text(sqlite3_stmt *stmt, int index, const char *text);
/** @brief 绑定 BLOB；SQLITE_TRANSIENT，会复制一份 */
esp_err_t esp_sqlite3_bind_blob(sqlite3_stmt *stmt, int index, const void *data, int len);

/** @brief 读取第 col 列文本并复制到 buf；NULL 值输出空串 */
esp_err_t esp_sqlite3_column_text_copy(sqlite3_stmt *stmt, int col,
                                      char *buf, size_t buf_len);

/* =========================================================================
 * 事务
 * ========================================================================= */

/** @brief BEGIN IMMEDIATE（避免读事务升级写事务时的死锁） */
esp_err_t esp_sqlite3_begin(esp_sqlite3_t *db);

/** @brief BEGIN DEFERRED */
esp_err_t esp_sqlite3_begin_deferred(esp_sqlite3_t *db);

/** @brief COMMIT */
esp_err_t esp_sqlite3_commit(esp_sqlite3_t *db);

/** @brief ROLLBACK（不在事务中时返回 ESP_OK） */
esp_err_t esp_sqlite3_rollback(esp_sqlite3_t *db);

/**
 * @brief 在事务中执行 fn
 *
 * fn 返回 ESP_OK -> COMMIT；否则 ROLLBACK 并把 fn 的错误码返回。
 * 适合“批量写入”场景，减少 Flash 擦写次数。
 */
esp_err_t esp_sqlite3_transaction(esp_sqlite3_t *db, esp_sqlite3_txn_cb_t fn, void *ctx);

/* =========================================================================
 * 维护与诊断
 * ========================================================================= */

/**
 * @brief 完整性检查
 * @param quick true = PRAGMA quick_check（快，但不如 integrity_check 全面）
 * @param out 输出结果（"ok" 或错误详情），可为 NULL
 */
esp_err_t esp_sqlite3_integrity_check(esp_sqlite3_t *db, bool quick,
                                     char *out, size_t out_len);

/** @brief VACUUM 重建数据库，回收空间 */
esp_err_t esp_sqlite3_vacuum(esp_sqlite3_t *db);

/** @brief VACUUM INTO 另一个文件（SQLite 3.27+），用于一致性备份 */
esp_err_t esp_sqlite3_vacuum_into(esp_sqlite3_t *db, const char *dest_path);

/**
 * @brief WAL checkpoint
 * @param mode 0=PASSIVE, 1=FULL, 2=RESTART, 3=TRUNCATE
 */
esp_err_t esp_sqlite3_wal_checkpoint(esp_sqlite3_t *db, int mode,
                                    int *log_frames, int *checkpointed_frames);

/** @brief PRAGMA optimize（长期运行设备建议关机前或每日执行一次） */
esp_err_t esp_sqlite3_optimize(esp_sqlite3_t *db);

/** @brief ANALYZE，生成查询规划器统计信息 */
esp_err_t esp_sqlite3_analyze(esp_sqlite3_t *db);

/**
 * @brief 用 SQLite 在线备份 API 复制数据库
 *
 * 源库与目标库都通过本组件打开；目标库已存在时会被覆盖内容。
 */
esp_err_t esp_sqlite3_backup(const char *src_path, const char *dst_path);

/** @brief 查询数据库文件大小（字节） */
esp_err_t esp_sqlite3_get_size(esp_sqlite3_t *db, int64_t *bytes);

/** @brief 读取 PRAGMA user_version */
esp_err_t esp_sqlite3_get_user_version(esp_sqlite3_t *db, int *version);

/** @brief 写入 PRAGMA user_version */
esp_err_t esp_sqlite3_set_user_version(esp_sqlite3_t *db, int version);

/** @brief 判断表是否存在（不区分大小写） */
esp_err_t esp_sqlite3_table_exists(esp_sqlite3_t *db, const char *table, bool *exists);

/** @brief 设置 busy timeout */
esp_err_t esp_sqlite3_set_busy_timeout(esp_sqlite3_t *db, uint32_t ms);

/** @brief 获取 SQLite 内存统计 */
esp_err_t esp_sqlite3_get_mem_stats(esp_sqlite3_mem_stats_t *out);

/**
 * @brief 执行数据库迁移
 *
 * 读取当前 user_version，按 steps 的顺序执行 version 大于当前值的步骤，
 * 每一步都在独立事务中执行；全部成功后 user_version 更新为最后一个版本。
 * steps 必须按 version 递增排列。
 *
 * 示例：
 * @code
 *   static const esp_sqlite3_migration_t steps[] = {
 *       { 1, "CREATE TABLE device(id INTEGER PRIMARY KEY, name TEXT);" },
 *       { 2, "ALTER TABLE device ADD COLUMN online INTEGER DEFAULT 0;" },
 *   };
 *   esp_sqlite3_migrate(db, steps, sizeof(steps)/sizeof(steps[0]));
 * @endcode
 */
esp_err_t esp_sqlite3_migrate(esp_sqlite3_t *db,
                             const esp_sqlite3_migration_t *steps, size_t count);

/* =========================================================================
 * 错误处理
 * ========================================================================= */

const char *esp_sqlite3_errmsg(const esp_sqlite3_t *db);
int esp_sqlite3_errcode(const esp_sqlite3_t *db);
int esp_sqlite3_extended_errcode(const esp_sqlite3_t *db);
/** @brief 最近一次 SQL 语法错误的字节偏移（SQLite 3.38+），未知返回 -1 */
int esp_sqlite3_error_offset(const esp_sqlite3_t *db);
const char *esp_sqlite3_errstr(int rc);
/** @brief 把 SQLite 结果码转换成 esp_err_t */
esp_err_t esp_sqlite3_rc_to_esp_err(int rc);

/* =========================================================================
 * 自测（仅 CONFIG_ESP_SQLITE3_SELFTEST=y 时链接到实现）
 * ========================================================================= */

/**
 * @brief 运行内置自测：建表/写入/查询/事务/JSON/FTS5/RTREE/
 *        shox96/完整性检查/备份/迁移。
 *
 * @param db_path 数据库路径，调用者需保证所在文件系统已挂载
 * @return ESP_OK 全部通过；其他值表示第一处失败
 */
esp_err_t esp_sqlite3_run_selftest(const char *db_path);

#ifdef __cplusplus
}
#endif
