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

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
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

static bool s_initialized = false;

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
    if (s_initialized) {
        return ESP_OK;
    }

    /* 顺序非常重要：MALLOC/MUTEX/LOG 都必须在 sqlite3_initialize() 之前配置 */
    esp_err_t err = esp_sqlite3_mem_register();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_sqlite3_mutex_register();
    if (err != ESP_OK) {
        return err;
    }
    int rc = sqlite3_config(SQLITE_CONFIG_LOG, esp_sqlite3_log_cb, NULL);
    if (rc != SQLITE_OK) {
        ESP_SQLITE3_LOGW("sqlite3_config(LOG) failed: rc=%d", rc);
    }

    /* SQLITE_OS_OTHER=1 时，sqlite3_initialize() 会回调 sqlite3_os_init() 注册 VFS */
    rc = sqlite3_initialize();
    if (rc != SQLITE_OK) {
        ESP_SQLITE3_LOGE("sqlite3_initialize failed: rc=%d (%s)", rc, sqlite3_errstr(rc));
        return ESP_FAIL;
    }

    /* 临时文件目录（仅当编译为 temp_store=FILE 时需要） */
#if !CONFIG_ESP_SQLITE3_TEMP_STORE_MEMORY
    if (sqlite3_temp_directory == NULL) {
        sqlite3_temp_directory = sqlite3_mprintf("%s", CONFIG_ESP_SQLITE3_TEMP_DIR);
        if (sqlite3_temp_directory == NULL) {
            ESP_SQLITE3_LOGW("set temp directory failed (out of memory)");
        }
    }
#endif

    err = esp_sqlite3_ext_register();
    if (err != ESP_OK) {
        ESP_SQLITE3_LOGW("register extensions failed");
        /* 扩展失败不阻止核心数据库使用 */
    }

    s_initialized = true;
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
    return ESP_OK;
}

void esp_sqlite3_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    esp_sqlite3_ext_unregister();

    int rc = sqlite3_shutdown();
    if (rc != SQLITE_OK) {
        /* 仍有连接/语句未关闭：保持 initialized 状态，避免后续 open 崩溃 */
        ESP_SQLITE3_LOGW("sqlite3_shutdown failed: rc=%d (%s)", rc, sqlite3_errstr(rc));
        return;
    }
    s_initialized = false;
}

bool esp_sqlite3_is_initialized(void)
{
    return s_initialized;
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
    return s_initialized ? ESP_OK : esp_sqlite3_init();
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

    esp_err_t err = esp_sqlite3_ensure_init();
    if (err != ESP_OK) {
        return err;
    }

    esp_sqlite3_t *wrapper = calloc(1, sizeof(*wrapper));
    if (wrapper == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wrapper->path = strdup(cfg->path);
    if (wrapper->path == NULL) {
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
            sqlite3_close_v2(wrapper->db);
        }
        free(wrapper->path);
        free(wrapper);
        return esp_sqlite3_rc_to_esp_err(rc);
    }

    /* 扩展错误码：sqlite3_extended_errcode() 才有意义 */
    sqlite3_extended_result_codes(wrapper->db, 1);
    sqlite3_busy_timeout(wrapper->db, (int)cfg->busy_timeout_ms);

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

    if (!cfg->read_only && cfg->journal_mode != ESP_SQLITE3_JOURNAL_DEFAULT) {
        char sql[64];
        snprintf(sql, sizeof(sql), "PRAGMA journal_mode=%s;",
                 esp_sqlite3_journal_mode_name(cfg->journal_mode));
        esp_sqlite3_apply_pragma(wrapper->db, sql);
    }

    {
        char sql[64];
        snprintf(sql, sizeof(sql), "PRAGMA synchronous=%d;", (int)cfg->synchronous);
        esp_sqlite3_apply_pragma(wrapper->db, sql);
    }

    esp_sqlite3_apply_pragma(wrapper->db, cfg->foreign_keys
                    ? "PRAGMA foreign_keys=ON;"
                    : "PRAGMA foreign_keys=OFF;");

    esp_sqlite3_apply_pragma(wrapper->db, cfg->temp_store_memory
                    ? "PRAGMA temp_store=MEMORY;"
                    : "PRAGMA temp_store=FILE;");

    if (cfg->exclusive_locking && !cfg->read_only) {
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
                    cfg->path, cfg->read_only,
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

void esp_sqlite3_close(esp_sqlite3_t *db)
{
    if (db == NULL) {
        return;
    }

    if (db->db != NULL) {
        int rc = sqlite3_close_v2(db->db);
        if (rc != SQLITE_OK) {
            esp_sqlite3_log_error(db->db, rc, "sqlite3_close_v2");
        }
        db->db = NULL;
    }

    free(db->path);
    free(db);
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
        ESP_SQLITE3_LOGE("exec failed: rc=%d msg=%s sql=%s", rc,
                        errmsg != NULL ? errmsg : sqlite3_errmsg(db->db), sql);
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
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
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
    int rc = sqlite3_prepare_v2(esp_sqlite3_handle(db), sql, -1, &stmt, NULL);
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
    int rc = sqlite3_prepare_v2(esp_sqlite3_handle(db), sql, -1, &stmt, NULL);
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
    int rc = sqlite3_prepare_v2(esp_sqlite3_handle(db), sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(esp_sqlite3_handle(db), rc, "prepare(text)");
        return esp_sqlite3_rc_to_esp_err(rc);
    }

    esp_err_t result;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (text == NULL) {
            if (buf != NULL && buf_len > 0) {
                buf[0] = '\0';
            }
            result = ESP_OK;
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
    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(db->db, rc, "sqlite3_prepare_v2");
        return esp_sqlite3_rc_to_esp_err(rc);
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

    const unsigned char *text = sqlite3_column_text(stmt, col);
    if (text == NULL) {
        buf[0] = '\0';
        return ESP_OK;
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
    char *errmsg = NULL;
    int rc = sqlite3_exec(db->db, sql, esp_sqlite3_integrity_cb, &ctx, &errmsg);
    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(db->db, rc, "integrity_check");
        sqlite3_free(errmsg);
        return esp_sqlite3_rc_to_esp_err(rc);
    }
    sqlite3_free(errmsg);

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

esp_err_t esp_sqlite3_vacuum(esp_sqlite3_t *db)
{
    return esp_sqlite3_exec(db, "VACUUM;");
}

esp_err_t esp_sqlite3_vacuum_into(esp_sqlite3_t *db, const char *dest_path)
{
    if (dest_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* VACUUM INTO 的目标路径由 SQLite 直接交给 xOpen，先按 VFS 规则补全 */
    char full[CONFIG_ESP_SQLITE3_MAX_PATH];
    esp_err_t path_err = esp_sqlite3_vfs_make_path(dest_path, full, sizeof(full));
    if (path_err != ESP_OK) {
        return path_err;
    }

    char *sql = sqlite3_mprintf("VACUUM INTO %Q;", full);
    if (sql == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_sqlite3_exec(db, sql);
    sqlite3_free(sql);
    return err;
}

esp_err_t esp_sqlite3_wal_checkpoint(esp_sqlite3_t *db, int mode,
                                    int *log_frames, int *checkpointed_frames)
{
    if (db == NULL || db->db == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

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
    if (strcmp(src_path, dst_path) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_sqlite3_t *src = NULL;
    esp_sqlite3_t *dst = NULL;

    /* 源库必须存在：create=false，避免把拼错的路径当成空库备份出去 */
    esp_sqlite3_config_t src_cfg;
    esp_sqlite3_config_init(&src_cfg);
    src_cfg.path = src_path;
    src_cfg.create = false;
    esp_err_t err = esp_sqlite3_open(&src_cfg, &src);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_sqlite3_open_simple(dst_path, &dst);
    if (err != ESP_OK) {
        esp_sqlite3_close(src);
        return err;
    }

    sqlite3_backup *backup = sqlite3_backup_init(esp_sqlite3_handle(dst), "main",
                                                 esp_sqlite3_handle(src), "main");
    if (backup == NULL) {
        esp_sqlite3_log_error(esp_sqlite3_handle(dst), sqlite3_errcode(esp_sqlite3_handle(dst)),
                             "sqlite3_backup_init");
        esp_sqlite3_close(dst);
        esp_sqlite3_close(src);
        return ESP_FAIL;
    }

    int rc;
    int retries = 0;
    do {
        rc = sqlite3_backup_step(backup, 128);  /* 分块，避免长时间占用 CPU */
        if (rc == SQLITE_OK) {
            retries = 0;
            taskYIELD();
        } else if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            /* 最多重试约 1 秒，避免在 BUSY 上无限自旋 */
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

    esp_sqlite3_close(dst);
    esp_sqlite3_close(src);
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
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 COLLATE NOCASE LIMIT 1;",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        esp_sqlite3_log_error(db->db, rc, "prepare(table_exists)");
        return esp_sqlite3_rc_to_esp_err(rc);
    }

    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
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
    return esp_sqlite3_rc_to_esp_err(sqlite3_busy_timeout(db->db, (int)ms));
}

esp_err_t esp_sqlite3_get_mem_stats(esp_sqlite3_mem_stats_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

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
}

esp_err_t esp_sqlite3_migrate(esp_sqlite3_t *db,
                             const esp_sqlite3_migration_t *steps, size_t count)
{
    if (db == NULL || (steps == NULL && count > 0)) {
        return ESP_ERR_INVALID_ARG;
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
