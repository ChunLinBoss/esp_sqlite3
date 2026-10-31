/**
 * @file esp_sqlite3.c
 * @brief SQLite3 高层封装：初始化、打开、执行、事务、维护、迁移
 *
 * 代码风格约定
 * ------------
 * - 所有对外函数先做参数检查，再确保运行时已初始化；
 * - 返回 esp_err_t 的接口内部统一调用 esp_sqlite3_rc_to_esp_err()；
 * - 出错时记录 sqlite3_errmsg()/扩展错误码/错误偏移，便于定位 SQL 问题；
 * - 不吞掉 SQLITE_BUSY/LOCKED，交给上层的 busy_timeout 或业务重试。
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sqlite3.h"

#include "esp_sqlite3.h"
#include "esp_sqlite3_internal.h"
#include "esp_sqlite3_vfs.h"

/* -------------------------------------------------------------------------
 * 内部状态
 * ------------------------------------------------------------------------- */

struct esp_sqlite3 {
    sqlite3 *db;        /**< 底层连接 */
    char *path;         /**< 打开路径副本 */
    bool read_only;     /**< 是否只读打开 */
};

typedef enum {
    ESP_SQLITE3_STATE_UNINIT = 0,
    ESP_SQLITE3_STATE_INITIALIZING,
    ESP_SQLITE3_STATE_READY,
} esp_sqlite3_state_t;

static volatile esp_sqlite3_state_t s_state = ESP_SQLITE3_STATE_UNINIT;
static int s_open_connections = 0;
static SemaphoreHandle_t s_state_mutex = NULL;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t esp_sqlite3_state_mutex_get(void)
{
    if (s_state_mutex == NULL) {
        portENTER_CRITICAL(&s_state_mux);
        if (s_state_mutex == NULL) {
            s_state_mutex = xSemaphoreCreateRecursiveMutex();
        }
        portEXIT_CRITICAL(&s_state_mux);
    }
    return s_state_mutex;
}

static esp_err_t esp_sqlite3_connection_begin(void)
{
    SemaphoreHandle_t lock = esp_sqlite3_state_mutex_get();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    esp_err_t err = esp_sqlite3_init();
    if (err == ESP_OK) {
        s_open_connections++;
    }
    xSemaphoreGiveRecursive(lock);
    return err;
}

static void esp_sqlite3_connection_end(void)
{
    SemaphoreHandle_t lock = esp_sqlite3_state_mutex_get();
    if (lock == NULL) {
        return;
    }
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    if (s_open_connections > 0) {
        s_open_connections--;
    }
    xSemaphoreGiveRecursive(lock);
}

/* -------------------------------------------------------------------------
 * 工具函数
 * ------------------------------------------------------------------------- */

static const char *esp_sqlite3_journal_mode_name(esp_sqlite3_journal_mode_t mode)
{
    switch (mode) {
    case ESP_SQLITE3_JOURNAL_DEFAULT: return NULL; /* 不改动 */
    case ESP_SQLITE3_JOURNAL_DELETE: return "DELETE";
    case ESP_SQLITE3_JOURNAL_TRUNCATE: return "TRUNCATE";
    case ESP_SQLITE3_JOURNAL_PERSIST: return "PERSIST";
    case ESP_SQLITE3_JOURNAL_MEMORY: return "MEMORY";
    case ESP_SQLITE3_JOURNAL_WAL: return "WAL";
    case ESP_SQLITE3_JOURNAL_OFF: return "OFF";
    default: return "DELETE";
    }
}

static int esp_sqlite3_prepare_retry(sqlite3 *db, const char *sql, int sql_len,
                                     sqlite3_stmt **stmt, const char **tail)
{
    int rc = sqlite3_prepare_v2(db, sql, sql_len, stmt, tail);
    if (rc == SQLITE_NOMEM) {
        ESP_SQLITE3_LOGW("prepare NOMEM, release db memory and retry");
        (void)sqlite3_db_release_memory(db);
        if (stmt != NULL) {
            *stmt = NULL;
        }
        rc = sqlite3_prepare_v2(db, sql, sql_len, stmt, tail);
    }
    return rc;
}

/** 执行可选 PRAGMA：失败只告警，不影响打开（不同文件系统支持度不同） */
static void esp_sqlite3_apply_pragma(sqlite3 *db, const char *sql)
{
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        ESP_SQLITE3_LOGW("PRAGMA failed: %s -> %s", sql,
                        errmsg != NULL ? errmsg : sqlite3_errstr(rc));
    }
    sqlite3_free(errmsg);
}

/** 执行必需 PRAGMA：失败返回错误 */
static esp_err_t esp_sqlite3_apply_pragma_required(sqlite3 *db, const char *sql)
{
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        ESP_SQLITE3_LOGE("PRAGMA failed: %s -> %s", sql,
                        errmsg != NULL ? errmsg : sqlite3_errstr(rc));
        sqlite3_free(errmsg);
        return esp_sqlite3_rc_to_esp_err(rc);
    }
    sqlite3_free(errmsg);
    return ESP_OK;
}

/** 查询单行单列 PRAGMA 文本结果 */
static esp_err_t esp_sqlite3_pragma_text(sqlite3 *db, const char *sql,
                                        char *out, size_t out_len)
{
    if (db == NULL || sql == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    sqlite3_stmt *stmt = NULL;
    int rc = esp_sqlite3_prepare_retry(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(db, rc, sql);
        return esp_sqlite3_rc_to_esp_err(rc);
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (text != NULL) {
            snprintf(out, out_len, "%s", (const char *)text);
        }
    } else if (rc != SQLITE_DONE) {
        esp_sqlite3_log_error(db, rc, sql);
        sqlite3_finalize(stmt);
        return esp_sqlite3_rc_to_esp_err(rc);
    }
    sqlite3_finalize(stmt);
    return out[0] != '\0' ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/** 查询单行单列 PRAGMA 整数结果 */
static esp_err_t esp_sqlite3_pragma_int(sqlite3 *db, const char *sql, int64_t *out)
{
    if (db == NULL || sql == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = esp_sqlite3_prepare_retry(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(db, rc, sql);
        return esp_sqlite3_rc_to_esp_err(rc);
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(stmt, 0);
    } else {
        esp_sqlite3_log_error(db, rc, sql);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE ? ESP_ERR_NOT_FOUND : esp_sqlite3_rc_to_esp_err(rc);
    }
    sqlite3_finalize(stmt);
    return ESP_OK;
}

/** 打开失败时释放 wrapper，并撤销连接计数 */
static void esp_sqlite3_abort_open(esp_sqlite3_t *wrapper)
{
    if (wrapper == NULL) {
        return;
    }
    if (wrapper->db != NULL) {
        (void)sqlite3_close(wrapper->db);
        wrapper->db = NULL;
    }
    esp_sqlite3_connection_end();
    free(wrapper->path);
    free(wrapper);
}

/** 内部：把 SQLite 错误写入日志 */
void esp_sqlite3_log_error(sqlite3 *db, int rc, const char *what)
{
    if (db != NULL) {
        int ext = sqlite3_extended_errcode(db);
        int off = sqlite3_error_offset(db);
        if (off >= 0) {
            ESP_SQLITE3_LOGE("%s failed: rc=%d ext=%d off=%d msg=%s",
                            what, rc, ext, off, sqlite3_errmsg(db));
        } else {
            ESP_SQLITE3_LOGE("%s failed: rc=%d ext=%d msg=%s",
                            what, rc, ext, sqlite3_errmsg(db));
        }
    } else {
        ESP_SQLITE3_LOGE("%s failed: rc=%d (%s)", what, rc, sqlite3_errstr(rc));
    }
}

/** SQLite 内部日志回调 */
static void esp_sqlite3_log_cb(void *arg, int code, const char *msg)
{
    (void)arg;
#ifdef CONFIG_ESP_SQLITE3_LOG_ERRORS
    if (code != SQLITE_OK && code != SQLITE_NOTICE) {
        ESP_SQLITE3_LOGE("internal: code=%d msg=%s", code, msg != NULL ? msg : "");
    } else {
        ESP_SQLITE3_LOGD("internal: code=%d msg=%s", code, msg != NULL ? msg : "");
    }
#else
    (void)code;
    (void)msg;
#endif
}

/* -------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */

esp_err_t esp_sqlite3_init(void)
{
    SemaphoreHandle_t lock = esp_sqlite3_state_mutex_get();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTakeRecursive(lock, portMAX_DELAY);

    if (s_state == ESP_SQLITE3_STATE_READY) {
        xSemaphoreGiveRecursive(lock);
        return ESP_OK;
    }
    if (s_state == ESP_SQLITE3_STATE_INITIALIZING) {
        /* 同一任务递归调用 init；不允许在初始化途中再次配置 SQLite */
        xSemaphoreGiveRecursive(lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_state = ESP_SQLITE3_STATE_INITIALIZING;

    /* 顺序非常重要：MALLOC/MUTEX/LOG 都必须在 sqlite3_initialize() 之前配置 */
    esp_err_t err = esp_sqlite3_mem_register();
    if (err == ESP_ERR_INVALID_STATE) {
        /*
         * 用户可能在 esp_sqlite3_init() 之前直接调用了 sqlite3_initialize()。
         * 此时 SQLite 已用默认分配器/NOOP 互斥量完成初始化，必须 shutdown
         * 后重新配置；如果还有外部连接存在，shutdown 会返回 BUSY，保持原错误。
         */
        ESP_LOGW(ESP_SQLITE3_TAG,
                 "SQLite was initialized externally; resetting runtime configuration");
        int shutdown_rc = sqlite3_shutdown();
        if (shutdown_rc == SQLITE_OK) {
            (void)esp_sqlite3_vfs_reset();
            err = esp_sqlite3_mem_register();
        } else {
            ESP_SQLITE3_LOGE("sqlite3_shutdown after external init failed: rc=%d", shutdown_rc);
        }
    }
    if (err != ESP_OK) {
        s_state = ESP_SQLITE3_STATE_UNINIT;
        xSemaphoreGiveRecursive(lock);
        return err;
    }

    err = esp_sqlite3_mutex_register();
    if (err != ESP_OK) {
        s_state = ESP_SQLITE3_STATE_UNINIT;
        xSemaphoreGiveRecursive(lock);
        return err;
    }
    int rc = sqlite3_config(SQLITE_CONFIG_LOG, esp_sqlite3_log_cb, NULL);
    if (rc != SQLITE_OK) {
        ESP_SQLITE3_LOGW("sqlite3_config(LOG) failed: rc=%d", rc);
    }

    /* SQLITE_OS_OTHER=1 时，sqlite3_initialize() 会回调 sqlite3_os_init()；
     * VFS 在 initialize 完成后注册，避免 sqlite3_vfs_register() 递归初始化。 */
    rc = sqlite3_initialize();
    if (rc != SQLITE_OK) {
        ESP_SQLITE3_LOGE("sqlite3_initialize failed: rc=%d (%s)", rc, sqlite3_errstr(rc));
        s_state = ESP_SQLITE3_STATE_UNINIT;
        xSemaphoreGiveRecursive(lock);
        return ESP_FAIL;
    }

    err = esp_sqlite3_vfs_register(true);
    if (err != ESP_OK) {
        ESP_SQLITE3_LOGE("register VFS failed: %s", esp_err_to_name(err));
        (void)sqlite3_shutdown();
        s_state = ESP_SQLITE3_STATE_UNINIT;
        xSemaphoreGiveRecursive(lock);
        return err;
    }

    /*
     * 临时文件目录必须始终设置，不能只在编译默认 temp_store=FILE 时设置：
     * 运行时可传 cfg.temp_store_memory=false 并执行 PRAGMA temp_store=FILE，
     * 此时若 sqlite3_temp_directory==NULL，临时库会落到 VFS 当前目录，
     * 在 SPIFFS 上通常直接 CANTOPEN。
     */
    if (sqlite3_temp_directory == NULL && CONFIG_ESP_SQLITE3_TEMP_DIR[0] != '\0') {
        sqlite3_temp_directory = sqlite3_mprintf("%s", CONFIG_ESP_SQLITE3_TEMP_DIR);
        if (sqlite3_temp_directory == NULL) {
            ESP_SQLITE3_LOGW("set temp directory failed (out of memory)");
        }
    }

    err = esp_sqlite3_ext_register();
    if (err != ESP_OK) {
        ESP_SQLITE3_LOGW("register extensions failed");
        /* 扩展失败不阻止核心数据库使用 */
    }

    s_state = ESP_SQLITE3_STATE_READY;
#if defined(CONFIG_ESP_SQLITE3_TEMP_STORE_MEMORY) && CONFIG_ESP_SQLITE3_TEMP_STORE_MEMORY
    const char *temp_store_mode = "memory";
#else
    const char *temp_store_mode = "file";
#endif
    ESP_SQLITE3_LOGI("SQLite %s initialized (vfs=%s, temp_store=%s, threadsafe=%d)",
                    sqlite3_libversion(), CONFIG_ESP_SQLITE3_VFS_NAME,
                    temp_store_mode,
#if CONFIG_ESP_SQLITE3_THREADSAFE
                    1
#else
                    0
#endif
    );
    xSemaphoreGiveRecursive(lock);
    return ESP_OK;
}

esp_err_t esp_sqlite3_deinit(void)
{
    SemaphoreHandle_t lock = esp_sqlite3_state_mutex_get();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    if (s_state != ESP_SQLITE3_STATE_READY) {
        xSemaphoreGiveRecursive(lock);
        return ESP_OK;
    }
    if (s_open_connections != 0) {
        ESP_SQLITE3_LOGE("deinit refused: %d database connection(s) still open",
                        s_open_connections);
        xSemaphoreGiveRecursive(lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_sqlite3_vfs_stats_t vfs_stats;
    esp_sqlite3_vfs_get_stats(&vfs_stats);
    if (vfs_stats.open_files != 0) {
        ESP_SQLITE3_LOGE("deinit refused: %u VFS file(s) still open",
                        (unsigned)vfs_stats.open_files);
        xSemaphoreGiveRecursive(lock);
        return ESP_ERR_INVALID_STATE;
    }

    esp_sqlite3_ext_unregister();

    int rc = sqlite3_shutdown();
    if (rc != SQLITE_OK) {
        ESP_SQLITE3_LOGW("sqlite3_shutdown failed: rc=%d (%s)", rc, sqlite3_errstr(rc));
        xSemaphoreGiveRecursive(lock);
        return esp_sqlite3_rc_to_esp_err(rc);
    }
    s_state = ESP_SQLITE3_STATE_UNINIT;
    if (esp_sqlite3_vfs_reset() != ESP_OK) {
        ESP_SQLITE3_LOGW("VFS reset failed after shutdown");
    }
    xSemaphoreGiveRecursive(lock);
    return ESP_OK;
}

bool esp_sqlite3_is_initialized(void)
{
    return s_state == ESP_SQLITE3_STATE_READY;
}

bool esp_sqlite3_is_initializing(void)
{
    return s_state == ESP_SQLITE3_STATE_INITIALIZING;
}

const char *esp_sqlite3_version(void)
{
    return sqlite3_libversion();
}

void esp_sqlite3_set_log_level(int level)
{
    esp_log_level_set(ESP_SQLITE3_TAG, (esp_log_level_t)level);
}

esp_err_t esp_sqlite3_ensure_init(void)
{
    if (s_state == ESP_SQLITE3_STATE_READY) {
        return ESP_OK;
    }
    return esp_sqlite3_init();
}

/* -------------------------------------------------------------------------
 * 配置与打开
 * ------------------------------------------------------------------------- */

void esp_sqlite3_config_init(esp_sqlite3_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->path = NULL;
    cfg->vfs = NULL;
    cfg->read_only = false;
    cfg->create = true;
    cfg->uri = false;
    cfg->busy_timeout_ms = CONFIG_ESP_SQLITE3_BUSY_TIMEOUT_MS;
    cfg->cache_size_kib = 0;   /* 0 = 使用编译默认值 */
    cfg->page_size = 0;        /* 0 = 使用编译默认值 */
    cfg->journal_mode = ESP_SQLITE3_JOURNAL_DEFAULT;
#ifdef SQLITE_DEFAULT_SYNCHRONOUS
    cfg->synchronous = (esp_sqlite3_sync_mode_t)SQLITE_DEFAULT_SYNCHRONOUS;
#else
    cfg->synchronous = ESP_SQLITE3_SYNC_FULL;
#endif
#if defined(CONFIG_ESP_SQLITE3_FOREIGN_KEYS) && CONFIG_ESP_SQLITE3_FOREIGN_KEYS
    cfg->foreign_keys = true;
#else
    cfg->foreign_keys = false;
#endif
#if defined(CONFIG_ESP_SQLITE3_TEMP_STORE_MEMORY) && CONFIG_ESP_SQLITE3_TEMP_STORE_MEMORY
    cfg->temp_store_memory = true;
#else
    cfg->temp_store_memory = false;
#endif
    cfg->exclusive_locking = false;
    cfg->defensive = false;
    cfg->query_only = false;
    cfg->auto_optimize = false;
}

esp_err_t esp_sqlite3_open(const esp_sqlite3_config_t *cfg, esp_sqlite3_t **out_db)
{
    if (cfg == NULL || cfg->path == NULL || out_db == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_db = NULL;

    /* 参数合法性检查：SQLite 要求页大小为 512..8192 的 2 的幂 */
    if (cfg->page_size != 0 &&
        (cfg->page_size < 512 || cfg->page_size > 8192 ||
         (cfg->page_size & (cfg->page_size - 1)) != 0)) {
        ESP_SQLITE3_LOGE("invalid page_size: %lu", (unsigned long)cfg->page_size);
        return ESP_ERR_INVALID_ARG;
    }
    if ((int)cfg->synchronous < 0 || (int)cfg->synchronous > 3) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((int)cfg->journal_mode < (int)ESP_SQLITE3_JOURNAL_DEFAULT ||
        (int)cfg->journal_mode > (int)ESP_SQLITE3_JOURNAL_OFF) {
        return ESP_ERR_INVALID_ARG;
    }

#ifdef SQLITE_OMIT_WAL
    if (cfg->journal_mode == ESP_SQLITE3_JOURNAL_WAL) {
        ESP_SQLITE3_LOGE("WAL is not compiled in this profile");
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
#ifdef SQLITE_OMIT_FOREIGN_KEY
    if (cfg->foreign_keys) {
        ESP_SQLITE3_LOGE("foreign keys are not compiled in this profile");
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif

    esp_err_t err = esp_sqlite3_connection_begin();
    if (err != ESP_OK) {
        return err;
    }

    esp_sqlite3_t *wrapper = calloc(1, sizeof(*wrapper));
    if (wrapper == NULL) {
        esp_sqlite3_connection_end();
        return ESP_ERR_NO_MEM;
    }

    wrapper->path = strdup(cfg->path);
    if (wrapper->path == NULL) {
        esp_sqlite3_connection_end();
        free(wrapper);
        return ESP_ERR_NO_MEM;
    }
    wrapper->read_only = cfg->read_only;

    int flags = cfg->read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
    if (!cfg->read_only && cfg->create) {
        flags |= SQLITE_OPEN_CREATE;
    }
    if (cfg->uri) {
        flags |= SQLITE_OPEN_URI;
    }

    int rc = sqlite3_open_v2(cfg->path, &wrapper->db, flags,
                             cfg->vfs != NULL ? cfg->vfs : NULL);
    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(wrapper->db, rc, "sqlite3_open_v2");
        if (wrapper->db != NULL) {
            (void)sqlite3_close(wrapper->db);
        }
        esp_sqlite3_connection_end();
        free(wrapper->path);
        free(wrapper);
        return esp_sqlite3_rc_to_esp_err(rc);
    }

    /* 扩展错误码：sqlite3_extended_errcode() 才有意义 */
    sqlite3_extended_result_codes(wrapper->db, 1);
    int busy_timeout = (cfg->busy_timeout_ms > (uint32_t)INT32_MAX)
                           ? INT32_MAX : (int)cfg->busy_timeout_ms;
    sqlite3_busy_timeout(wrapper->db, busy_timeout);

    /* VFS 可能在写保护介质上把 READWRITE 降级为 O_RDONLY。 */
    if (sqlite3_db_readonly(wrapper->db, "main") == 1) {
        if (!wrapper->read_only) {
            ESP_SQLITE3_LOGW("database opened read-only by filesystem: %s", cfg->path);
        }
        wrapper->read_only = true;
    }

#ifdef SQLITE_OMIT_WAL
    /* OMIT_WAL 固件打开 WAL 主库时不会回放 -wal，必须拒绝而不是读到旧数据 */
    if (!cfg->uri && cfg->journal_mode != ESP_SQLITE3_JOURNAL_WAL) {
        char full[CONFIG_ESP_SQLITE3_MAX_PATH + 16];
        if (esp_sqlite3_vfs_make_path(cfg->path, full, sizeof(full)) == ESP_OK) {
            int probe = open(full, O_RDONLY);
            if (probe >= 0) {
                uint8_t hdr[20];
                ssize_t n = read(probe, hdr, sizeof(hdr));
                close(probe);
                if (n >= 20 && hdr[18] == 2 && hdr[19] == 2) {
                    ESP_SQLITE3_LOGE("database is in WAL mode but this build has SQLITE_OMIT_WAL");
                    esp_sqlite3_abort_open(wrapper);
                    return ESP_ERR_NOT_SUPPORTED;
                }
            }
        }
    }
#endif

    /* ---- 连接级 PRAGMA -------------------------------------------------- */
    if (cfg->cache_size_kib > 0) {
        char sql[64];
        snprintf(sql, sizeof(sql), "PRAGMA cache_size=-%lu;",
                 (unsigned long)cfg->cache_size_kib);
        esp_sqlite3_apply_pragma(wrapper->db, sql);
    }

    if (cfg->page_size > 0) {
        char sql[64];
        snprintf(sql, sizeof(sql), "PRAGMA page_size=%lu;", (unsigned long)cfg->page_size);
        esp_sqlite3_apply_pragma(wrapper->db, sql);
    }

    if (!wrapper->read_only && cfg->journal_mode != ESP_SQLITE3_JOURNAL_DEFAULT) {
        char sql[64];
        snprintf(sql, sizeof(sql), "PRAGMA journal_mode=%s;",
                 esp_sqlite3_journal_mode_name(cfg->journal_mode));
        err = esp_sqlite3_apply_pragma_required(wrapper->db, sql);
        if (err != ESP_OK) {
            esp_sqlite3_abort_open(wrapper);
            return err;
        }

        char actual[32] = {0};
        err = esp_sqlite3_pragma_text(wrapper->db, "PRAGMA journal_mode;",
                                      actual, sizeof(actual));
        const char *wanted = esp_sqlite3_journal_mode_name(cfg->journal_mode);
        if (err != ESP_OK || strcasecmp(actual, wanted) != 0) {
            ESP_SQLITE3_LOGE("journal_mode not applied: wanted=%s actual=%s",
                            wanted, actual);
            esp_sqlite3_abort_open(wrapper);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    {
        char sql[64];
        snprintf(sql, sizeof(sql), "PRAGMA synchronous=%d;", (int)cfg->synchronous);
        err = esp_sqlite3_apply_pragma_required(wrapper->db, sql);
        if (err != ESP_OK) {
            esp_sqlite3_abort_open(wrapper);
            return err;
        }
        int64_t actual_sync = -1;
        if (esp_sqlite3_pragma_int(wrapper->db, "PRAGMA synchronous;", &actual_sync) == ESP_OK &&
            actual_sync != (int64_t)cfg->synchronous) {
            ESP_SQLITE3_LOGW("synchronous not applied: wanted=%d actual=%lld",
                            (int)cfg->synchronous, (long long)actual_sync);
        }
    }

#ifndef SQLITE_OMIT_FOREIGN_KEY
    err = esp_sqlite3_apply_pragma_required(wrapper->db, cfg->foreign_keys
                    ? "PRAGMA foreign_keys=ON;"
                    : "PRAGMA foreign_keys=OFF;");
    if (err != ESP_OK) {
        esp_sqlite3_abort_open(wrapper);
        return err;
    }
    {
        int64_t actual_fk = -1;
        if (esp_sqlite3_pragma_int(wrapper->db, "PRAGMA foreign_keys;", &actual_fk) == ESP_OK &&
            actual_fk != (cfg->foreign_keys ? 1 : 0)) {
            ESP_SQLITE3_LOGE("foreign_keys not applied: wanted=%d actual=%lld",
                            cfg->foreign_keys ? 1 : 0, (long long)actual_fk);
            esp_sqlite3_abort_open(wrapper);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
#endif

    esp_sqlite3_apply_pragma(wrapper->db, cfg->temp_store_memory
                    ? "PRAGMA temp_store=MEMORY;"
                    : "PRAGMA temp_store=FILE;");

    if (cfg->exclusive_locking && !wrapper->read_only) {
        esp_sqlite3_apply_pragma(wrapper->db, "PRAGMA locking_mode=EXCLUSIVE;");
    }

    if (cfg->query_only || cfg->read_only) {
        esp_sqlite3_apply_pragma(wrapper->db, "PRAGMA query_only=ON;");
    }

    if (cfg->defensive) {
        int on = 1;
        int drc = sqlite3_db_config(wrapper->db, SQLITE_DBCONFIG_DEFENSIVE, on, NULL);
        if (drc != SQLITE_OK) {
            ESP_SQLITE3_LOGW("enable defensive mode failed: rc=%d", drc);
        }
    }

    if (cfg->auto_optimize) {
        (void)esp_sqlite3_apply_pragma_required(wrapper->db, "PRAGMA optimize;");
    }

    const char *journal_name = esp_sqlite3_journal_mode_name(cfg->journal_mode);
    ESP_SQLITE3_LOGI("opened %s (read_only=%d, journal=%s, sync=%d, cache=%luKiB)",
                    cfg->path, wrapper->read_only,
                    journal_name != NULL ? journal_name : "keep",
                    (int)cfg->synchronous, (unsigned long)cfg->cache_size_kib);

    *out_db = wrapper;
    return ESP_OK;
}

esp_err_t esp_sqlite3_open_simple(const char *path, esp_sqlite3_t **out_db)
{
    esp_sqlite3_config_t cfg;
    esp_sqlite3_config_init(&cfg);
    cfg.path = path;
    return esp_sqlite3_open(&cfg, out_db);
}

esp_err_t esp_sqlite3_close(esp_sqlite3_t *db)
{
    if (db == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (db->db != NULL) {
        int rc = sqlite3_close(db->db);
        if (rc != SQLITE_OK) {
            esp_sqlite3_log_error(db->db, rc, "sqlite3_close");
            /* close 的 BUSY 是“还有未 finalize 语句”，不是超时 */
            return (rc == SQLITE_BUSY) ? ESP_ERR_INVALID_STATE
                                       : esp_sqlite3_rc_to_esp_err(rc);
        }
        db->db = NULL;
        esp_sqlite3_connection_end();
    }

    free(db->path);
    free(db);
    return ESP_OK;
}

sqlite3 *esp_sqlite3_handle(esp_sqlite3_t *db)
{
    return db != NULL ? db->db : NULL;
}

const char *esp_sqlite3_path(const esp_sqlite3_t *db)
{
    return db != NULL ? db->path : NULL;
}

bool esp_sqlite3_in_transaction(const esp_sqlite3_t *db)
{
    if (db == NULL || db->db == NULL) {
        return false;
    }
    return sqlite3_get_autocommit(db->db) == 0;
}

/* -------------------------------------------------------------------------
 * 执行 SQL
 * ------------------------------------------------------------------------- */

esp_err_t esp_sqlite3_exec(esp_sqlite3_t *db, const char *sql)
{
    if (db == NULL || db->db == NULL || sql == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *errmsg = NULL;
    int rc = sqlite3_exec(db->db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        ESP_SQLITE3_LOGE("exec failed: rc=%d msg=%s", rc,
                        errmsg != NULL ? errmsg : sqlite3_errmsg(db->db));
    }
    sqlite3_free(errmsg);
    return esp_sqlite3_rc_to_esp_err(rc);
}

esp_err_t esp_sqlite3_exec_cb(esp_sqlite3_t *db, const char *sql,
                             int (*callback)(void *, int, char **, char **),
                             void *arg)
{
    if (db == NULL || db->db == NULL || sql == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *errmsg = NULL;
    int rc = sqlite3_exec(db->db, sql, callback, arg, &errmsg);
    if (rc != SQLITE_OK) {
        ESP_SQLITE3_LOGE("exec_cb failed: rc=%d msg=%s", rc,
                        errmsg != NULL ? errmsg : sqlite3_errmsg(db->db));
    }
    sqlite3_free(errmsg);
    return esp_sqlite3_rc_to_esp_err(rc);
}

esp_err_t esp_sqlite3_query(esp_sqlite3_t *db, const char *sql,
                           esp_sqlite3_row_cb_t cb, void *ctx)
{
    if (db == NULL || db->db == NULL || sql == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sqlite3_stmt *stmt = NULL;
    int rc = esp_sqlite3_prepare_retry(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(db->db, rc, "prepare(query)");
        return esp_sqlite3_rc_to_esp_err(rc);
    }

    esp_err_t result = ESP_OK;
    int row = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (cb != NULL) {
            esp_err_t cb_rc = cb(ctx, stmt, row);
            if (cb_rc != ESP_OK) {
                result = cb_rc;
                break;
            }
        }
        row++;
    }

    if (result == ESP_OK && rc != SQLITE_DONE) {
        esp_sqlite3_log_error(db->db, rc, "step(query)");
        result = esp_sqlite3_rc_to_esp_err(rc);
    }

    sqlite3_finalize(stmt);
    return result;
}

esp_err_t esp_sqlite3_query_int64(esp_sqlite3_t *db, const char *sql, int64_t *out)
{
    if (db == NULL || db->db == NULL || sql == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sqlite3_stmt *stmt = NULL;
    int rc = esp_sqlite3_prepare_retry(esp_sqlite3_handle(db), sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(esp_sqlite3_handle(db), rc, "prepare(int64)");
        return esp_sqlite3_rc_to_esp_err(rc);
    }

    esp_err_t result;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) == SQLITE_NULL) {
            result = ESP_ERR_NOT_FOUND;
        } else {
            *out = sqlite3_column_int64(stmt, 0);
            result = ESP_OK;
        }
    } else if (rc == SQLITE_DONE) {
        result = ESP_ERR_NOT_FOUND;
    } else {
        esp_sqlite3_log_error(esp_sqlite3_handle(db), rc, "step(int64)");
        result = esp_sqlite3_rc_to_esp_err(rc);
    }

    sqlite3_finalize(stmt);
    return result;
}

esp_err_t esp_sqlite3_query_double(esp_sqlite3_t *db, const char *sql, double *out)
{
    if (db == NULL || db->db == NULL || sql == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sqlite3_stmt *stmt = NULL;
    int rc = esp_sqlite3_prepare_retry(esp_sqlite3_handle(db), sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(esp_sqlite3_handle(db), rc, "prepare(double)");
        return esp_sqlite3_rc_to_esp_err(rc);
    }

    esp_err_t result;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) == SQLITE_NULL) {
            result = ESP_ERR_NOT_FOUND;
        } else {
            *out = sqlite3_column_double(stmt, 0);
            result = ESP_OK;
        }
    } else if (rc == SQLITE_DONE) {
        result = ESP_ERR_NOT_FOUND;
    } else {
        esp_sqlite3_log_error(esp_sqlite3_handle(db), rc, "step(double)");
        result = esp_sqlite3_rc_to_esp_err(rc);
    }

    sqlite3_finalize(stmt);
    return result;
}

esp_err_t esp_sqlite3_query_text(esp_sqlite3_t *db, const char *sql,
                                char *buf, size_t buf_len)
{
    if (db == NULL || db->db == NULL || sql == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sqlite3_stmt *stmt = NULL;
    int rc = esp_sqlite3_prepare_retry(esp_sqlite3_handle(db), sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(esp_sqlite3_handle(db), rc, "prepare(text)");
        return esp_sqlite3_rc_to_esp_err(rc);
    }

    esp_err_t result;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) == SQLITE_NULL) {
            if (buf != NULL && buf_len > 0) {
                buf[0] = '\0';
            }
            result = ESP_OK;
        } else {
            const unsigned char *text = sqlite3_column_text(stmt, 0);
            if (text == NULL) {
                result = ESP_ERR_NO_MEM;
            } else {
                size_t len = strlen((const char *)text);
                if (buf == NULL) {
                    result = ESP_OK;
                } else if (len + 1 > buf_len) {
                    result = ESP_ERR_INVALID_SIZE;
                } else {
                    memcpy(buf, text, len + 1);
                    result = ESP_OK;
                }
            }
        }
    } else if (rc == SQLITE_DONE) {
        result = ESP_ERR_NOT_FOUND;
    } else {
        esp_sqlite3_log_error(esp_sqlite3_handle(db), rc, "step(text)");
        result = esp_sqlite3_rc_to_esp_err(rc);
    }

    sqlite3_finalize(stmt);
    return result;
}

/* -------------------------------------------------------------------------
 * 预编译语句
 * ------------------------------------------------------------------------- */

esp_err_t esp_sqlite3_prepare(esp_sqlite3_t *db, const char *sql, sqlite3_stmt **stmt)
{
    return esp_sqlite3_prepare_v2(db, sql, -1, stmt, NULL);
}

esp_err_t esp_sqlite3_prepare_v2(esp_sqlite3_t *db, const char *sql, int sql_len,
                                sqlite3_stmt **stmt, const char **tail)
{
    if (db == NULL || db->db == NULL || sql == NULL || stmt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int rc = sqlite3_prepare_v2(db->db, sql, sql_len, stmt, tail);
    if (rc == SQLITE_NOMEM) {
        /*
         * 小内存设备常见：连接还能用，但页缓存/空闲块碎片化导致编译语句
         * 拿不到内存。先释放本连接可回收的页缓存，再原地重试一次；这是
         * 安全的，因为 prepare 失败不会改变数据库内容。
         */
        ESP_LOGW(ESP_SQLITE3_TAG, "prepare NOMEM, release db memory and retry");
        (void)sqlite3_db_release_memory(db->db);
        if (stmt != NULL) {
            *stmt = NULL;
        }
        rc = sqlite3_prepare_v2(db->db, sql, sql_len, stmt, tail);
    }
    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(db->db, rc, "sqlite3_prepare_v2");
        return esp_sqlite3_rc_to_esp_err(rc);
    }
    if (*stmt == NULL) {
        /* 纯空白/注释 SQL：SQLite 返回 OK 但不产生语句 */
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

int esp_sqlite3_step(sqlite3_stmt *stmt)
{
    if (stmt == NULL) {
        return SQLITE_MISUSE;
    }
    return sqlite3_step(stmt);
}

esp_err_t esp_sqlite3_reset(sqlite3_stmt *stmt)
{
    return esp_sqlite3_rc_to_esp_err(sqlite3_reset(stmt));
}

esp_err_t esp_sqlite3_clear_bindings(sqlite3_stmt *stmt)
{
    if (stmt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_sqlite3_rc_to_esp_err(sqlite3_clear_bindings(stmt));
}

esp_err_t esp_sqlite3_finalize(sqlite3_stmt *stmt)
{
    return esp_sqlite3_rc_to_esp_err(sqlite3_finalize(stmt));
}

esp_err_t esp_sqlite3_bind_null(sqlite3_stmt *stmt, int index)
{
    return esp_sqlite3_rc_to_esp_err(sqlite3_bind_null(stmt, index));
}

esp_err_t esp_sqlite3_bind_int(sqlite3_stmt *stmt, int index, int value)
{
    return esp_sqlite3_rc_to_esp_err(sqlite3_bind_int(stmt, index, value));
}

esp_err_t esp_sqlite3_bind_int64(sqlite3_stmt *stmt, int index, int64_t value)
{
    return esp_sqlite3_rc_to_esp_err(sqlite3_bind_int64(stmt, index, value));
}

esp_err_t esp_sqlite3_bind_double(sqlite3_stmt *stmt, int index, double value)
{
    return esp_sqlite3_rc_to_esp_err(sqlite3_bind_double(stmt, index, value));
}

esp_err_t esp_sqlite3_bind_text(sqlite3_stmt *stmt, int index, const char *text)
{
    /* SQLITE_TRANSIENT：让 SQLite 复制一份，调用者无需保持 text 生命周期 */
    return esp_sqlite3_rc_to_esp_err(
        sqlite3_bind_text(stmt, index, text, -1, SQLITE_TRANSIENT));
}

esp_err_t esp_sqlite3_bind_blob(sqlite3_stmt *stmt, int index, const void *data, int len)
{
    return esp_sqlite3_rc_to_esp_err(
        sqlite3_bind_blob(stmt, index, data, len, SQLITE_TRANSIENT));
}

esp_err_t esp_sqlite3_column_text_copy(sqlite3_stmt *stmt, int col,
                                      char *buf, size_t buf_len)
{
    if (stmt == NULL || buf == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (col < 0 || col >= sqlite3_column_count(stmt)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
        buf[0] = '\0';
        return ESP_OK;
    }

    const unsigned char *text = sqlite3_column_text(stmt, col);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t len = strlen((const char *)text);
    if (len + 1 > buf_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buf, text, len + 1);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * 事务
 * ------------------------------------------------------------------------- */

esp_err_t esp_sqlite3_begin(esp_sqlite3_t *db)
{
    return esp_sqlite3_exec(db, "BEGIN IMMEDIATE;");
}

esp_err_t esp_sqlite3_begin_deferred(esp_sqlite3_t *db)
{
    return esp_sqlite3_exec(db, "BEGIN DEFERRED;");
}

esp_err_t esp_sqlite3_commit(esp_sqlite3_t *db)
{
    return esp_sqlite3_exec(db, "COMMIT;");
}

esp_err_t esp_sqlite3_rollback(esp_sqlite3_t *db)
{
    if (db == NULL || db->db == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sqlite3_get_autocommit(db->db) != 0) {
        return ESP_OK;  /* 不在事务中 */
    }
    return esp_sqlite3_exec(db, "ROLLBACK;");
}

esp_err_t esp_sqlite3_transaction(esp_sqlite3_t *db, esp_sqlite3_txn_cb_t fn, void *ctx)
{
    if (db == NULL || fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_sqlite3_begin(db);
    if (err != ESP_OK) {
        return err;
    }

    err = fn(ctx);
    if (err != ESP_OK) {
        (void)esp_sqlite3_rollback(db);
        return err;
    }

    err = esp_sqlite3_commit(db);
    if (err != ESP_OK) {
        (void)esp_sqlite3_rollback(db);
        return err;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * 维护与诊断
 * ------------------------------------------------------------------------- */

typedef struct {
    char *out;
    size_t out_len;
    bool captured;
} esp_sqlite3_integrity_ctx_t;

static int esp_sqlite3_integrity_cb(void *arg, int argc, char **argv, char **col)
{
    (void)col;
    esp_sqlite3_integrity_ctx_t *ctx = (esp_sqlite3_integrity_ctx_t *)arg;
    if (ctx == NULL || argc < 1 || ctx->captured) {
        return 0;
    }
    ctx->captured = true;
    if (ctx->out != NULL && ctx->out_len > 0) {
        snprintf(ctx->out, ctx->out_len, "%s", argv[0] != NULL ? argv[0] : "");
    }
    return 0;
}

esp_err_t esp_sqlite3_integrity_check(esp_sqlite3_t *db, bool quick,
                                     char *out, size_t out_len)
{
    if (db == NULL || db->db == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 即使调用者不关心结果，也要捕获第一行来判断是否 "ok" */
    char local_buf[256] = {0};
    esp_sqlite3_integrity_ctx_t ctx = {
        .out = local_buf,
        .out_len = sizeof(local_buf),
        .captured = false,
    };

    const char *sql = quick ? "PRAGMA quick_check;" : "PRAGMA integrity_check;";
#ifdef SQLITE_OMIT_INTEGRITY_CHECK
    (void)sql;
    ESP_SQLITE3_LOGW("integrity_check is not compiled in this profile");
    return ESP_ERR_NOT_SUPPORTED;
#else
    char *errmsg = NULL;
    int rc = sqlite3_exec(db->db, sql, esp_sqlite3_integrity_cb, &ctx, &errmsg);
    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(db->db, rc, "integrity_check");
        sqlite3_free(errmsg);
        return esp_sqlite3_rc_to_esp_err(rc);
    }
    sqlite3_free(errmsg);
#endif

    if (out != NULL && out_len > 0) {
        snprintf(out, out_len, "%s", local_buf);
    }

    if (!ctx.captured || strcmp(local_buf, "ok") != 0) {
        ESP_SQLITE3_LOGE("integrity check failed: %s",
                        ctx.captured ? local_buf : "(no result)");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** 当前任务未订阅 TWDT 时，esp_task_wdt_reset() 会打印错误，先查询状态 */
static inline void esp_sqlite3_wdt_feed(void)
{
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        (void)esp_task_wdt_reset();
    }
}

/** VACUUM 等长操作的进度回调：喂任务看门狗并周期性让出 CPU */
#ifndef SQLITE_OMIT_PROGRESS_CALLBACK
static int esp_sqlite3_long_op_progress(void *arg)
{
    uint32_t *count = (uint32_t *)arg;
    if (count != NULL) {
        (*count)++;
        if ((*count & 0x3F) == 0) {
            vTaskDelay(1);
        }
    }
    esp_sqlite3_wdt_feed();
    return 0;
}
#endif

static esp_err_t esp_sqlite3_exec_long(esp_sqlite3_t *db, const char *sql)
{
#ifndef SQLITE_OMIT_PROGRESS_CALLBACK
    if (db != NULL && db->db != NULL) {
        uint32_t progress = 0;
        sqlite3_progress_handler(db->db, 1000, esp_sqlite3_long_op_progress, &progress);
        esp_err_t err = esp_sqlite3_exec(db, sql);
        sqlite3_progress_handler(db->db, 0, NULL, NULL);
        return err;
    }
#endif
    return esp_sqlite3_exec(db, sql);
}

esp_err_t esp_sqlite3_vacuum(esp_sqlite3_t *db)
{
    return esp_sqlite3_exec_long(db, "VACUUM;");
}

esp_err_t esp_sqlite3_vacuum_into(esp_sqlite3_t *db, const char *dest_path)
{
    if (dest_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* VACUUM INTO 的目标路径由 SQLite 直接交给 xOpen，先按 VFS 规则补全 */
    char full[CONFIG_ESP_SQLITE3_MAX_PATH + 16];
    esp_err_t path_err = esp_sqlite3_vfs_make_path(dest_path, full, sizeof(full));
    if (path_err != ESP_OK) {
        return path_err;
    }

    char *sql = sqlite3_mprintf("VACUUM INTO %Q;", full);
    if (sql == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_sqlite3_exec_long(db, sql);
    sqlite3_free(sql);
    return err;
}

esp_err_t esp_sqlite3_wal_checkpoint(esp_sqlite3_t *db, int mode,
                                    int *log_frames, int *checkpointed_frames)
{
    if (db == NULL || db->db == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#ifdef SQLITE_OMIT_WAL
    (void)mode;
    if (log_frames != NULL) {
        *log_frames = 0;
    }
    if (checkpointed_frames != NULL) {
        *checkpointed_frames = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
#else
    int log_n = 0;
    int ckpt_n = 0;
    int rc = sqlite3_wal_checkpoint_v2(db->db, NULL, mode, &log_n, &ckpt_n);
    if (log_frames != NULL) {
        *log_frames = log_n;
    }
    if (checkpointed_frames != NULL) {
        *checkpointed_frames = ckpt_n;
    }
    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(db->db, rc, "wal_checkpoint");
    }
    return esp_sqlite3_rc_to_esp_err(rc);
#endif
}

esp_err_t esp_sqlite3_optimize(esp_sqlite3_t *db)
{
    return esp_sqlite3_exec(db, "PRAGMA optimize;");
}

esp_err_t esp_sqlite3_analyze(esp_sqlite3_t *db)
{
    return esp_sqlite3_exec(db, "ANALYZE;");
}

esp_err_t esp_sqlite3_backup(const char *src_path, const char *dst_path)
{
    if (src_path == NULL || dst_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char src_full[CONFIG_ESP_SQLITE3_MAX_PATH + 16];
    char dst_full[CONFIG_ESP_SQLITE3_MAX_PATH + 16];
    esp_err_t path_err = esp_sqlite3_vfs_make_path(src_path, src_full, sizeof(src_full));
    if (path_err != ESP_OK) {
        return path_err;
    }
    path_err = esp_sqlite3_vfs_make_path(dst_path, dst_full, sizeof(dst_full));
    if (path_err != ESP_OK) {
        return path_err;
    }
    if (strcmp(src_full, dst_full) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_sqlite3_t *src = NULL;
    esp_sqlite3_t *dst = NULL;

    /* 源库必须存在：create=false，避免把拼错的路径当成空库备份出去 */
    esp_sqlite3_config_t src_cfg;
    esp_sqlite3_config_init(&src_cfg);
    src_cfg.path = src_full;
    src_cfg.create = false;
    /*
     * 备份会同时打开源/目标两个连接。ESP32-C5 等无 PSRAM 的型号内部
     * RAM 只有约 300KiB，默认 64KiB page cache 两个连接就可能 OOM。
     * 备份是顺序读写，8KiB cache 足够，且能显著降低峰值内存。
     */
    src_cfg.cache_size_kib = 8;
    src_cfg.temp_store_memory = false;
    esp_err_t err = esp_sqlite3_open(&src_cfg, &src);
    if (err != ESP_OK) {
        return err;
    }

    esp_sqlite3_config_t dst_cfg;
    esp_sqlite3_config_init(&dst_cfg);
    dst_cfg.path = dst_full;
    dst_cfg.cache_size_kib = 8;
    dst_cfg.temp_store_memory = false;
    err = esp_sqlite3_open(&dst_cfg, &dst);
    if (err != ESP_OK) {
        esp_sqlite3_close(src);
        return err;
    }

    sqlite3_backup *backup = sqlite3_backup_init(esp_sqlite3_handle(dst), "main",
                                                 esp_sqlite3_handle(src), "main");
    if (backup == NULL) {
        int init_rc = sqlite3_errcode(esp_sqlite3_handle(dst));
        esp_sqlite3_log_error(esp_sqlite3_handle(dst), init_rc, "sqlite3_backup_init");
        esp_sqlite3_close(dst);
        esp_sqlite3_close(src);
        return esp_sqlite3_rc_to_esp_err(init_rc);
    }

    int rc;
    int retries = 0;
    unsigned steps = 0;
    do {
        /* 大库备份可能持续数秒：每步喂狗，周期性让 IDLE 任务运行 */
        esp_sqlite3_wdt_feed();
        if ((steps++ & 0x07) == 0) {
            vTaskDelay(1);
        }
        rc = sqlite3_backup_step(backup, 128);  /* 分块，避免长时间占用 CPU */
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            /* 累计最多重试约 1 秒，不能因为中途成功几次就无限延长 */
            if (++retries > 100) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);

    int finish_rc = sqlite3_backup_finish(backup);
    if (rc == SQLITE_DONE) {
        rc = finish_rc;
    }

    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(esp_sqlite3_handle(dst), rc, "sqlite3_backup");
    }

    esp_err_t close_dst_err = esp_sqlite3_close(dst);
    esp_err_t close_src_err = esp_sqlite3_close(src);
    if (rc == SQLITE_DONE || rc == SQLITE_OK) {
        if (close_dst_err != ESP_OK) {
            return close_dst_err;
        }
        if (close_src_err != ESP_OK) {
            return close_src_err;
        }
        return ESP_OK;
    }
    return esp_sqlite3_rc_to_esp_err(rc);
}

esp_err_t esp_sqlite3_get_size(esp_sqlite3_t *db, int64_t *bytes)
{
    if (bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t page_count = 0;
    int64_t page_size = 0;
    esp_err_t err = esp_sqlite3_query_int64(db, "PRAGMA page_count;", &page_count);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_sqlite3_query_int64(db, "PRAGMA page_size;", &page_size);
    if (err != ESP_OK) {
        return err;
    }
    *bytes = page_count * page_size;
    return ESP_OK;
}

esp_err_t esp_sqlite3_get_user_version(esp_sqlite3_t *db, int *version)
{
    if (version == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t v = 0;
    esp_err_t err = esp_sqlite3_query_int64(db, "PRAGMA user_version;", &v);
    if (err != ESP_OK) {
        return err;
    }
    *version = (int)v;
    return ESP_OK;
}

esp_err_t esp_sqlite3_set_user_version(esp_sqlite3_t *db, int version)
{
    char sql[64];
    snprintf(sql, sizeof(sql), "PRAGMA user_version=%d;", version);
    return esp_sqlite3_exec(db, sql);
}

esp_err_t esp_sqlite3_table_exists(esp_sqlite3_t *db, const char *table, bool *exists)
{
    if (db == NULL || db->db == NULL || table == NULL || exists == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *exists = false;
    sqlite3_stmt *stmt = NULL;
    int rc = esp_sqlite3_prepare_retry(db->db,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 COLLATE NOCASE LIMIT 1;",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(db->db, rc, "prepare(table_exists)");
        return esp_sqlite3_rc_to_esp_err(rc);
    }

    rc = sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(db->db, rc, "bind(table_exists)");
        sqlite3_finalize(stmt);
        return esp_sqlite3_rc_to_esp_err(rc);
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *exists = true;
        rc = SQLITE_OK;
    } else if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
    } else {
        esp_sqlite3_log_error(db->db, rc, "step(table_exists)");
    }

    sqlite3_finalize(stmt);
    return esp_sqlite3_rc_to_esp_err(rc);
}

esp_err_t esp_sqlite3_set_busy_timeout(esp_sqlite3_t *db, uint32_t ms)
{
    if (db == NULL || db->db == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int timeout = (ms > (uint32_t)INT32_MAX) ? INT32_MAX : (int)ms;
    return esp_sqlite3_rc_to_esp_err(sqlite3_busy_timeout(db->db, timeout));
}

esp_err_t esp_sqlite3_get_mem_stats(esp_sqlite3_mem_stats_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

#if !defined(SQLITE_DEFAULT_MEMSTATUS) || !SQLITE_DEFAULT_MEMSTATUS
    return ESP_ERR_NOT_SUPPORTED;
#else
    int64_t current = 0;
    int64_t high = 0;
    sqlite3_status64(SQLITE_STATUS_MEMORY_USED, &current, &high, 0);
    out->memory_used = current;
    out->memory_highwater = high;

    sqlite3_status64(SQLITE_STATUS_PAGECACHE_USED, &current, &high, 0);
    out->pagecache_used = current;
    out->pagecache_highwater = high;

    sqlite3_status64(SQLITE_STATUS_PAGECACHE_OVERFLOW, &current, &high, 0);
    out->pagecache_overflow = current;

    sqlite3_status64(SQLITE_STATUS_MALLOC_COUNT, &current, &high, 0);
    out->malloc_count = current;
    return ESP_OK;
#endif
}

esp_err_t esp_sqlite3_migrate(esp_sqlite3_t *db,
                             const esp_sqlite3_migration_t *steps, size_t count)
{
    if (db == NULL || (steps == NULL && count > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    int prev_version = 0;
    for (size_t i = 0; i < count; i++) {
        if (steps[i].version <= 0 || steps[i].version <= prev_version) {
            ESP_SQLITE3_LOGE("migration steps must have strictly increasing positive versions");
            return ESP_ERR_INVALID_ARG;
        }
        if (steps[i].sql == NULL) {
            ESP_SQLITE3_LOGE("migration step %d has NULL SQL", steps[i].version);
            return ESP_ERR_INVALID_ARG;
        }
        prev_version = steps[i].version;
    }

    int current = 0;
    esp_err_t err = esp_sqlite3_get_user_version(db, &current);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < count; i++) {
        if (steps[i].version <= current) {
            continue;
        }
        if (steps[i].sql == NULL) {
            return ESP_ERR_INVALID_ARG;
        }

        err = esp_sqlite3_begin(db);
        if (err != ESP_OK) {
            return err;
        }

        err = esp_sqlite3_exec(db, steps[i].sql);
        if (err == ESP_OK) {
            err = esp_sqlite3_set_user_version(db, steps[i].version);
        }

        if (err != ESP_OK) {
            (void)esp_sqlite3_rollback(db);
            ESP_SQLITE3_LOGE("migration to version %d failed", steps[i].version);
            return err;
        }

        err = esp_sqlite3_commit(db);
        if (err != ESP_OK) {
            (void)esp_sqlite3_rollback(db);
            return err;
        }

        current = steps[i].version;
        ESP_SQLITE3_LOGI("migrated to user_version=%d", current);
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * 错误码
 * ------------------------------------------------------------------------- */

const char *esp_sqlite3_errmsg(const esp_sqlite3_t *db)
{
    if (db == NULL || db->db == NULL) {
        return "database not open";
    }
    return sqlite3_errmsg(db->db);
}

int esp_sqlite3_errcode(const esp_sqlite3_t *db)
{
    if (db == NULL || db->db == NULL) {
        return SQLITE_MISUSE;
    }
    return sqlite3_errcode(db->db);
}

int esp_sqlite3_extended_errcode(const esp_sqlite3_t *db)
{
    if (db == NULL || db->db == NULL) {
        return SQLITE_MISUSE;
    }
    return sqlite3_extended_errcode(db->db);
}

int esp_sqlite3_error_offset(const esp_sqlite3_t *db)
{
    if (db == NULL || db->db == NULL) {
        return -1;
    }
    return sqlite3_error_offset(db->db);
}

const char *esp_sqlite3_errstr(int rc)
{
    return sqlite3_errstr(rc);
}

esp_err_t esp_sqlite3_rc_to_esp_err(int rc)
{
    switch (rc & 0xFF) {
    case SQLITE_OK:
    case SQLITE_ROW:
    case SQLITE_DONE:
        return ESP_OK;

    case SQLITE_NOMEM:
    case SQLITE_FULL:
        return ESP_ERR_NO_MEM;

    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return ESP_ERR_TIMEOUT;

    case SQLITE_NOTFOUND:
        return ESP_ERR_NOT_FOUND;

    case SQLITE_MISUSE:
    case SQLITE_RANGE:
        return ESP_ERR_INVALID_ARG;

    case SQLITE_CANTOPEN:
        return ESP_ERR_NOT_FOUND;

    case SQLITE_READONLY:
    case SQLITE_PERM:
    case SQLITE_AUTH:
        return ESP_ERR_NOT_ALLOWED;

    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
        return ESP_ERR_INVALID_CRC;

    case SQLITE_TOOBIG:
        return ESP_ERR_INVALID_SIZE;

    default:
        return ESP_FAIL;
    }
}
