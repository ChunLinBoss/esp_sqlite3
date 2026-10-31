/**
 * @file esp_sqlite3_selftest.c
 * @brief 内置自测：覆盖建表、事务、查询、JSON、FTS5、RTREE、shox96、
 *        完整性检查、VACUUM、备份、迁移、WAL checkpoint。
 *
 * 该文件只在 CONFIG_ESP_SQLITE3_SELFTEST=y 时参与编译，
 * 配套工程见 components/esp_sqlite3/test_apps/selftest。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "sqlite3.h"

#include "esp_sqlite3.h"
#include "esp_sqlite3_internal.h"
#include "esp_sqlite3_vfs.h"

#if CONFIG_ESP_SQLITE3_THREADSAFE
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#if CONFIG_ESP_SQLITE3_SELFTEST

#define TEST_TAG "esp_sqlite3_test"

/* 用于验证“FULL 创建 WAL 库 -> MINIMAL 打开必须拒绝”的跨固件探针 */
#define TEST_WAL_PROBE "/db/wal_downgrade_probe.db"

#define TEST_CHECK(cond, ...)                                              \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ESP_LOGE(TEST_TAG, "FAIL at %s:%d: ", __FILE__, __LINE__);     \
            ESP_LOGE(TEST_TAG, __VA_ARGS__);                               \
            result = ESP_FAIL;                                             \
            goto cleanup;                                                  \
        }                                                                  \
    } while (0)

#define TEST_STEP(expr)                                                    \
    do {                                                                   \
        esp_err_t __err = (expr);                                          \
        if (__err != ESP_OK) {                                             \
            ESP_LOGE(TEST_TAG, "FAIL at %s:%d: %s -> %s", __FILE__,        \
                     __LINE__, #expr, esp_err_to_name(__err));             \
            result = __err;                                                \
            goto cleanup;                                                  \
        }                                                                  \
    } while (0)

#if CONFIG_ESP_SQLITE3_THREADSAFE
typedef struct {
    const char *path;
    int count;
    volatile bool done;
    esp_err_t err;
} test_conc_ctx_t;

static void test_conc_task(void *arg)
{
    test_conc_ctx_t *ctx = (test_conc_ctx_t *)arg;
    esp_sqlite3_t *db = NULL;

    esp_sqlite3_config_t cfg;
    esp_sqlite3_config_init(&cfg);
    cfg.path = ctx->path;
    cfg.cache_size_kib = 4;
    cfg.busy_timeout_ms = 10000;
    cfg.temp_store_memory = true;

    ctx->err = esp_sqlite3_open(&cfg, &db);
    if (ctx->err == ESP_OK) {
        ctx->err = esp_sqlite3_begin(db);
        for (int i = 0; ctx->err == ESP_OK && i < ctx->count; i++) {
            char sql[64];
            snprintf(sql, sizeof(sql), "INSERT INTO conc(v) VALUES(%d);", i);
            ctx->err = esp_sqlite3_exec(db, sql);
        }
        if (ctx->err == ESP_OK) {
            ctx->err = esp_sqlite3_commit(db);
        } else {
            (void)esp_sqlite3_rollback(db);
        }
    }

    if (db != NULL) {
        esp_err_t close_err = esp_sqlite3_close(db);
        if (ctx->err == ESP_OK) {
            ctx->err = close_err;
        }
    }
    ctx->done = true;
    vTaskDelete(NULL);
}
#endif

/** 删除数据库及其日志/ WAL 边车文件 */
static void test_remove_db(const char *path)
{
    char buf[CONFIG_ESP_SQLITE3_MAX_PATH + 16];
    remove(path);
    snprintf(buf, sizeof(buf), "%s-journal", path);
    remove(buf);
    snprintf(buf, sizeof(buf), "%s-wal", path);
    remove(buf);
    snprintf(buf, sizeof(buf), "%s-shm", path);
    remove(buf);
}

static esp_err_t test_query_int64(esp_sqlite3_t *db, const char *sql, int64_t expected)
{
    int64_t value = 0;
    esp_err_t err = esp_sqlite3_query_int64(db, sql, &value);
    if (err != ESP_OK) {
        ESP_LOGE(TEST_TAG, "query failed: %s (%s)", sql, esp_err_to_name(err));
        return err;
    }
    if (value != expected) {
        ESP_LOGE(TEST_TAG, "unexpected value: %s -> %lld (expected %lld)", sql,
                 (long long)value, (long long)expected);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t esp_sqlite3_run_selftest(const char *db_path)
{
    esp_err_t result = ESP_FAIL;
    esp_sqlite3_t *db = NULL;
    sqlite3_stmt *stmt = NULL;
    char backup_path[CONFIG_ESP_SQLITE3_MAX_PATH + 8];

    if (db_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TEST_TAG, "===== esp_sqlite3 selftest start =====");
#if defined(CONFIG_ESP_SQLITE3_PROFILE_MAX)
    const char *profile = "MAX";
#elif defined(CONFIG_ESP_SQLITE3_PROFILE_FULL)
    const char *profile = "FULL";
#elif defined(CONFIG_ESP_SQLITE3_PROFILE_STANDARD)
    const char *profile = "STANDARD";
#elif defined(CONFIG_ESP_SQLITE3_PROFILE_MINIMAL)
    const char *profile = "MINIMAL";
#elif defined(CONFIG_ESP_SQLITE3_PROFILE_CRUD)
    const char *profile = "CRUD";
#else
    const char *profile = "UNKNOWN";
#endif
    ESP_LOGI(TEST_TAG, "PROFILE=%s", profile);
    ESP_LOGI(TEST_TAG, "SQLite compile options: %s", sqlite3_compileoption_get(0) ? "yes" : "none");

    /*
     * 特意先直接初始化 SQLite，验证 esp_sqlite3_init() 能检测并恢复外部
     * 初始化路径（否则默认 NOOP 互斥量会让多任务下的自测/业务不安全）。
     */
    TEST_CHECK(sqlite3_initialize() == SQLITE_OK, "external sqlite3_initialize failed");
    TEST_STEP(esp_sqlite3_init());

    /* 打印关键编译选项，方便确认档位是否生效 */
    for (int i = 0; sqlite3_compileoption_get(i) != NULL; i++) {
        ESP_LOGD(TEST_TAG, "compile_option: %s", sqlite3_compileoption_get(i));
    }

#if defined(SQLITE_OMIT_WAL)
    {
        /* 上一次非 OMIT_WAL 固件可能留下了 WAL 探针库 */
        esp_sqlite3_config_t legacy_cfg;
        esp_sqlite3_config_init(&legacy_cfg);
        legacy_cfg.path = TEST_WAL_PROBE;
        legacy_cfg.create = false;
        esp_sqlite3_t *legacy = NULL;
        esp_err_t legacy_err = esp_sqlite3_open(&legacy_cfg, &legacy);
        if (legacy_err == ESP_ERR_NOT_SUPPORTED) {
            TEST_CHECK(legacy == NULL, "WAL downgrade open returned a handle");
            ESP_LOGI(TEST_TAG, "WAL downgrade guard: ok");
        } else if (legacy_err == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TEST_TAG, "WAL downgrade probe missing (run a WAL profile first)");
        } else {
            TEST_CHECK(false, "WAL downgrade open unexpected: %s",
                       esp_err_to_name(legacy_err));
        }
        test_remove_db(TEST_WAL_PROBE);
    }
#else
    test_remove_db(TEST_WAL_PROBE);
#endif

    test_remove_db(db_path);

    esp_sqlite3_config_t cfg;
    esp_sqlite3_config_init(&cfg);
    cfg.path = db_path;
    cfg.create = true;
    cfg.busy_timeout_ms = 5000;
    cfg.cache_size_kib = 16;
    cfg.page_size = 4096;
#if CONFIG_ESP_SQLITE3_FOREIGN_KEYS
    cfg.foreign_keys = true;
#else
    cfg.foreign_keys = false;
#endif
    cfg.temp_store_memory = true;
#if CONFIG_ESP_SQLITE3_ENABLE_WAL
    cfg.journal_mode = ESP_SQLITE3_JOURNAL_WAL;
#else
    cfg.journal_mode = ESP_SQLITE3_JOURNAL_DELETE;
#endif
    cfg.synchronous = ESP_SQLITE3_SYNC_FULL;

    TEST_STEP(esp_sqlite3_open(&cfg, &db));
    ESP_LOGI(TEST_TAG, "opened %s, version=%s", db_path, esp_sqlite3_version());

    char journal[32] = {0};
    TEST_STEP(esp_sqlite3_query_text(db, "PRAGMA journal_mode;", journal, sizeof(journal)));
    ESP_LOGI(TEST_TAG, "journal_mode = %s", journal);
#if CONFIG_ESP_SQLITE3_ENABLE_WAL
    TEST_CHECK(strcasecmp(journal, "wal") == 0,
               "WAL profile compiled but journal_mode=%s", journal);
#endif

    /* ---- 1. 建表 -------------------------------------------------------- */
    TEST_STEP(esp_sqlite3_exec(db,
        "CREATE TABLE t("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  value REAL,"
        "  ts INTEGER,"
        "  payload BLOB"
        ");"));

    /* ---- 2. 事务批量写入 100 行 ---------------------------------------- */
    TEST_STEP(esp_sqlite3_begin(db));
    TEST_STEP(esp_sqlite3_prepare(db,
        "INSERT INTO t(name,value,ts,payload) VALUES(?1,?2,?3,?4);", &stmt));
    for (int i = 1; i <= 100; i++) {
        char name[32];
        snprintf(name, sizeof(name), "row-%03d", i);
        TEST_STEP(esp_sqlite3_bind_text(stmt, 1, name));
        TEST_STEP(esp_sqlite3_bind_double(stmt, 2, (double)i * 1.5));
        TEST_STEP(esp_sqlite3_bind_int64(stmt, 3, 1700000000LL + i));
        TEST_STEP(esp_sqlite3_bind_blob(stmt, 4, "payload", 7));

        int rc = esp_sqlite3_step(stmt);
        TEST_CHECK(rc == SQLITE_DONE, "insert row %d failed: rc=%d (%s)",
                   i, rc, sqlite3_errstr(rc));
        TEST_STEP(esp_sqlite3_reset(stmt));
    }
    {
        sqlite3_stmt *tmp = stmt;
        stmt = NULL;
        TEST_STEP(esp_sqlite3_finalize(tmp));
    }
    TEST_STEP(esp_sqlite3_commit(db));

    TEST_STEP(test_query_int64(db, "SELECT COUNT(*) FROM t;", 100));
    TEST_STEP(test_query_int64(db, "SELECT CAST(SUM(value) AS INTEGER) FROM t;", 7575));

    /* ---- 3. UPDATE / DELETE -------------------------------------------- */
    TEST_STEP(esp_sqlite3_exec(db, "UPDATE t SET value=value+1 WHERE id<=10;"));
    TEST_STEP(test_query_int64(db, "SELECT CAST(SUM(value) AS INTEGER) FROM t;", 7585));
    TEST_STEP(esp_sqlite3_exec(db, "DELETE FROM t WHERE id=100;"));
    TEST_STEP(test_query_int64(db, "SELECT COUNT(*) FROM t;", 99));
    TEST_STEP(test_query_int64(db, "SELECT COUNT(*) FROM t WHERE id=100;", 0));

    /* ---- 4. JSON（SQLite 3.38+ 内建） ---------------------------------- */
#if defined(SQLITE_OMIT_JSON)
    ESP_LOGW(TEST_TAG, "JSON: skipped (SQLITE_OMIT_JSON)");
#else
    TEST_STEP(test_query_int64(db,
        "SELECT json_extract('{\"a\":{\"b\":42}}', '$.a.b');", 42));
#endif

    /* ---- 5. FTS5 -------------------------------------------------------- */
#if defined(SQLITE_ENABLE_FTS5)
    TEST_STEP(esp_sqlite3_exec(db, "CREATE VIRTUAL TABLE ft USING fts5(content);"));
    TEST_STEP(esp_sqlite3_exec(db, "INSERT INTO ft VALUES('hello sqlite world');"));
    TEST_STEP(esp_sqlite3_exec(db, "INSERT INTO ft VALUES('embedded database');"));
    TEST_STEP(test_query_int64(db, "SELECT COUNT(*) FROM ft WHERE ft MATCH 'sqlite';", 1));
    ESP_LOGI(TEST_TAG, "FTS5: ok");
#else
    ESP_LOGW(TEST_TAG, "FTS5: skipped (not compiled)");
#endif

    /* ---- 6. RTREE ------------------------------------------------------- */
#if defined(SQLITE_ENABLE_RTREE)
    TEST_STEP(esp_sqlite3_exec(db,
        "CREATE VIRTUAL TABLE rt USING rtree(id, minx, maxx, miny, maxy);"));
    TEST_STEP(esp_sqlite3_exec(db, "INSERT INTO rt VALUES(1, 0, 10, 0, 10);"));
    TEST_STEP(test_query_int64(db,
        "SELECT COUNT(*) FROM rt WHERE minx<=5 AND maxx>=5 AND miny<=5 AND maxy>=5;", 1));
    ESP_LOGI(TEST_TAG, "RTREE: ok");
#else
    ESP_LOGW(TEST_TAG, "RTREE: skipped (not compiled)");
#endif

    /* ---- 7. shox96 兼容函数 -------------------------------------------- */
    {
        static const char *samples[] = {
            "",
            "a",
            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
            "Hello, SQLite 3.54.1!\nTab\tCR\r\n",
            "0123456789 0123456789 0123456789 0123456789",
            "The quick brown fox jumps over the lazy dog. "
            "The quick brown fox jumps over the lazy dog.",
        };
        for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
            char *sql = sqlite3_mprintf(
                "SELECT CAST(shox96_0_2d(shox96_0_2c(%Q)) AS TEXT) = %Q;",
                samples[i], samples[i]);
            TEST_CHECK(sql != NULL, "mprintf failed");
            esp_err_t err = test_query_int64(db, sql, 1);
            sqlite3_free(sql);
            TEST_CHECK(err == ESP_OK, "shox96 roundtrip %u failed", (unsigned)i);
        }
        ESP_LOGI(TEST_TAG, "shox96: ok");
    }

    /* ---- 8. 完整性检查 -------------------------------------------------- */
#if defined(SQLITE_OMIT_INTEGRITY_CHECK)
    ESP_LOGW(TEST_TAG, "integrity_check: skipped (SQLITE_OMIT_INTEGRITY_CHECK)");
#else
    {
        char check[256] = {0};
        TEST_STEP(esp_sqlite3_integrity_check(db, false, check, sizeof(check)));
        TEST_CHECK(strcasecmp(check, "ok") == 0, "integrity_check=%s", check);
        ESP_LOGI(TEST_TAG, "integrity_check = %s", check);
    }
#endif

    /* ---- 9. 备份 -------------------------------------------------------- */
    TEST_CHECK(strlen(db_path) + 5 < sizeof(backup_path), "db path too long");
    snprintf(backup_path, sizeof(backup_path), "%s.bak", db_path);
    test_remove_db(backup_path);
    TEST_STEP(esp_sqlite3_backup(db_path, backup_path));
    {
        esp_sqlite3_t *bak = NULL;
        TEST_STEP(esp_sqlite3_open_simple(backup_path, &bak));
        TEST_STEP(test_query_int64(bak, "SELECT COUNT(*) FROM t;", 99));
        esp_sqlite3_close(bak);
        ESP_LOGI(TEST_TAG, "backup: ok");
    }
    test_remove_db(backup_path);

    /* ---- 10. VACUUM ----------------------------------------------------- */
    TEST_STEP(esp_sqlite3_exec(db, "CREATE TABLE junk(x BLOB);"));
    TEST_STEP(esp_sqlite3_exec(db, "INSERT INTO junk VALUES(zeroblob(32768));"));
    TEST_STEP(esp_sqlite3_exec(db, "DELETE FROM junk;"));
    {
        int64_t free_before = 0;
        int64_t free_after = 0;
        TEST_STEP(esp_sqlite3_query_int64(db, "PRAGMA freelist_count;", &free_before));
        TEST_CHECK(free_before > 0, "freelist_count before VACUUM is 0");
        TEST_STEP(esp_sqlite3_vacuum(db));
        TEST_STEP(esp_sqlite3_query_int64(db, "PRAGMA freelist_count;", &free_after));
        TEST_CHECK(free_after < free_before, "VACUUM did not reduce freelist: %lld -> %lld",
                   (long long)free_before, (long long)free_after);
        TEST_STEP(test_query_int64(db, "SELECT COUNT(*) FROM t;", 99));
        ESP_LOGI(TEST_TAG, "VACUUM: ok (freelist %lld -> %lld)",
                 (long long)free_before, (long long)free_after);
    }

    /* ---- 11. user_version / 迁移 --------------------------------------- */
    TEST_STEP(esp_sqlite3_set_user_version(db, 0));
    {
        static const esp_sqlite3_migration_t steps[] = {
            { 1, "CREATE TABLE IF NOT EXISTS mig(id INTEGER PRIMARY KEY);" },
            { 2, "ALTER TABLE mig ADD COLUMN note TEXT;" },
        };
        TEST_STEP(esp_sqlite3_migrate(db, steps, sizeof(steps) / sizeof(steps[0])));
        int version = -1;
        TEST_STEP(esp_sqlite3_get_user_version(db, &version));
        TEST_CHECK(version == 2, "user_version=%d, expected 2", version);
        TEST_STEP(test_query_int64(db,
            "SELECT COUNT(*) FROM pragma_table_info('mig') WHERE name='note';", 1));
        ESP_LOGI(TEST_TAG, "migration: ok (user_version=%d)", version);
    }

    /* ---- 12. 文件大小与 WAL checkpoint --------------------------------- */
    {
        int64_t bytes = 0;
        TEST_STEP(esp_sqlite3_get_size(db, &bytes));
        TEST_CHECK(bytes > 0, "database size is 0");
        ESP_LOGI(TEST_TAG, "database size = %lld bytes", (long long)bytes);
    }

#if CONFIG_ESP_SQLITE3_ENABLE_WAL
    if (strcasecmp(journal, "wal") == 0) {
        int log_frames = 0;
        int ckpt_frames = 0;
        TEST_STEP(esp_sqlite3_wal_checkpoint(db, SQLITE_CHECKPOINT_FULL,
                                            &log_frames, &ckpt_frames));
        TEST_CHECK(log_frames > 0, "WAL checkpoint log_frames=0");
        TEST_CHECK(ckpt_frames == log_frames,
                   "WAL checkpoint incomplete: log=%d ckpt=%d", log_frames, ckpt_frames);
        ESP_LOGI(TEST_TAG, "wal_checkpoint: log=%d ckpt=%d", log_frames, ckpt_frames);
    }
#endif

    /* ---- 13. 边界 API 与泄漏回归 --------------------------------------- */
    {
        /* 同一文件别名必须被拒绝，不能自己备份自己 */
        TEST_CHECK(esp_sqlite3_backup(db_path, db_path) == ESP_ERR_INVALID_ARG,
                   "backup same path was not rejected");

        /* 空 SQL 不应返回可用的 stmt */
        sqlite3_stmt *empty_stmt = NULL;
        TEST_CHECK(esp_sqlite3_prepare(db, "  -- empty\n", &empty_stmt) == ESP_ERR_INVALID_ARG,
                   "empty SQL prepare should fail");
        TEST_CHECK(empty_stmt == NULL, "empty SQL returned non-NULL stmt");

        TEST_CHECK(esp_sqlite3_clear_bindings(NULL) == ESP_ERR_INVALID_ARG,
                   "clear_bindings(NULL) not rejected");

        /* query 系列对纯空白/注释 SQL 也必须返回错误，不能对 NULL stmt step */
        {
            int64_t dummy_i64 = 0;
            double dummy_d = 0;
            char dummy_t[8];
            TEST_CHECK(esp_sqlite3_query(db, " -- empty\n", NULL, NULL) ==
                       ESP_ERR_INVALID_ARG, "empty query SQL not rejected");
            TEST_CHECK(esp_sqlite3_query_int64(db, " -- empty\n", &dummy_i64) ==
                       ESP_ERR_INVALID_ARG, "empty query_int64 SQL not rejected");
            TEST_CHECK(esp_sqlite3_query_double(db, " -- empty\n", &dummy_d) ==
                       ESP_ERR_INVALID_ARG, "empty query_double SQL not rejected");
            TEST_CHECK(esp_sqlite3_query_text(db, " -- empty\n", dummy_t,
                                              sizeof(dummy_t)) ==
                       ESP_ERR_INVALID_ARG, "empty query_text SQL not rejected");
        }

        /* 非法迁移顺序必须在执行任何 SQL 前被拒绝 */
        static const esp_sqlite3_migration_t bad_steps[] = {
            { 2, "CREATE TABLE bad_order(x);" },
            { 1, "CREATE TABLE bad_order2(x);" },
        };
        int version_before = -1;
        TEST_STEP(esp_sqlite3_get_user_version(db, &version_before));
        TEST_CHECK(esp_sqlite3_migrate(db, bad_steps, 2) == ESP_ERR_INVALID_ARG,
                   "non-monotonic migration was accepted");
        int version_after = -1;
        TEST_STEP(esp_sqlite3_get_user_version(db, &version_after));
        TEST_CHECK(version_before == version_after,
                   "migration order check changed user_version");

        /* 压缩器必须拒绝非 ASCII，避免静默丢字节 */
        esp_err_t ascii_err = esp_sqlite3_exec(db, "SELECT shox96_0_2c('中文');");
        TEST_CHECK(ascii_err != ESP_OK, "shox96 accepted non-ASCII input");

        /* close 的 BUSY 契约：未 finalize 时必须返回 INVALID_STATE */
        sqlite3_stmt *busy_stmt = NULL;
        TEST_STEP(esp_sqlite3_prepare(db, "SELECT 1;", &busy_stmt));
        TEST_CHECK(esp_sqlite3_close(db) == ESP_ERR_INVALID_STATE,
                   "close with live statement did not return INVALID_STATE");
        TEST_STEP(esp_sqlite3_finalize(busy_stmt));

        /* 反复 open/close，检查没有 fd/连接泄漏 */
        for (int i = 0; i < 10; i++) {
            esp_sqlite3_t *tmp = NULL;
            TEST_STEP(esp_sqlite3_open_simple(db_path, &tmp));
            TEST_CHECK(tmp != NULL, "open_simple returned NULL");
            TEST_STEP(esp_sqlite3_close(tmp));
        }
        ESP_LOGI(TEST_TAG, "edge API/open-close: ok");
    }

    /* ---- 13b. VFS 稀疏写/短读/临时文件 ------------------------------- */
    {
        sqlite3_vfs *vfs = esp_sqlite3_vfs_get();
        TEST_CHECK(vfs != NULL && vfs->szOsFile > 0, "invalid VFS");
        sqlite3_file *vf = sqlite3_malloc64((sqlite3_uint64)vfs->szOsFile);
        TEST_CHECK(vf != NULL, "VFS file allocation failed");
        memset(vf, 0, vfs->szOsFile);

        int vrc = vfs->xOpen(vfs, "/db/vfs_io.tmp", vf,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                             SQLITE_OPEN_DELETEONCLOSE, NULL);
        TEST_CHECK(vrc == SQLITE_OK, "VFS xOpen sparse file failed: rc=%d", vrc);
        TEST_CHECK(vf->pMethods != NULL, "VFS pMethods is NULL");

        uint8_t data[16];
        memset(data, 0xA5, sizeof(data));
        TEST_CHECK(vf->pMethods->xWrite(vf, data, sizeof(data), 8192) == SQLITE_OK,
                   "VFS sparse xWrite failed");
        sqlite3_int64 vfs_size = 0;
        TEST_CHECK(vf->pMethods->xFileSize(vf, &vfs_size) == SQLITE_OK &&
                   vfs_size == 8208, "VFS sparse size=%lld", (long long)vfs_size);

        uint8_t read_buf[64];
        memset(read_buf, 0xCC, sizeof(read_buf));
        TEST_CHECK(vf->pMethods->xRead(vf, read_buf, sizeof(read_buf), 8192) ==
                   SQLITE_IOERR_SHORT_READ, "VFS EOF short read not reported");
        for (int i = 0; i < 16; i++) {
            TEST_CHECK(read_buf[i] == 0xA5, "VFS read data[%d]=0x%02x", i, read_buf[i]);
        }
        for (int i = 16; i < (int)sizeof(read_buf); i++) {
            TEST_CHECK(read_buf[i] == 0, "VFS short read tail[%d] not zero", i);
        }

        memset(read_buf, 0xCC, sizeof(read_buf));
        TEST_CHECK(vf->pMethods->xRead(vf, read_buf, sizeof(read_buf), 16384) ==
                   SQLITE_IOERR_SHORT_READ, "VFS far EOF short read not reported");
        for (int i = 0; i < (int)sizeof(read_buf); i++) {
            TEST_CHECK(read_buf[i] == 0, "VFS far EOF buffer[%d] not zero", i);
        }

        TEST_CHECK(vf->pMethods->xTruncate(vf, 0) == SQLITE_OK, "VFS xTruncate failed");
        TEST_CHECK(vf->pMethods->xFileSize(vf, &vfs_size) == SQLITE_OK && vfs_size == 0,
                   "VFS truncate size=%lld", (long long)vfs_size);
        TEST_CHECK(vf->pMethods->xClose(vf) == SQLITE_OK, "VFS xClose failed");
        sqlite3_free(vf);

        /* path=NULL 必须生成唯一临时文件并可正常关闭/删除 */
        vf = sqlite3_malloc64((sqlite3_uint64)vfs->szOsFile);
        TEST_CHECK(vf != NULL, "VFS temp file allocation failed");
        memset(vf, 0, vfs->szOsFile);
        vrc = vfs->xOpen(vfs, NULL, vf,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                         SQLITE_OPEN_DELETEONCLOSE, NULL);
        TEST_CHECK(vrc == SQLITE_OK, "VFS temp xOpen failed: rc=%d", vrc);
        TEST_CHECK(vf->pMethods->xWrite(vf, "temp", 4, 0) == SQLITE_OK,
                   "VFS temp write failed");
        TEST_CHECK(vf->pMethods->xFileSize(vf, &vfs_size) == SQLITE_OK && vfs_size == 4,
                   "VFS temp size=%lld", (long long)vfs_size);
        TEST_CHECK(vf->pMethods->xClose(vf) == SQLITE_OK, "VFS temp xClose failed");
        sqlite3_free(vf);

        char escaped[CONFIG_ESP_SQLITE3_MAX_PATH + 16];
        TEST_CHECK(esp_sqlite3_vfs_make_path("../escape.db", escaped, sizeof(escaped)) ==
                   ESP_ERR_INVALID_ARG, "VFS relative path escaped default dir");
        ESP_LOGI(TEST_TAG, "VFS sparse/short-read/temp: ok");
    }

    /* ---- 13c. 大小写不同的文件名必须保持独立（SPIFFS/LittleFS） ------- */
    {
        const char *case_a = "/db/CaseA.db";
        const char *case_b = "/db/casea.db";
        esp_sqlite3_t *db_a = NULL;
        esp_sqlite3_t *db_b = NULL;
        test_remove_db(case_a);
        test_remove_db(case_b);

        TEST_STEP(esp_sqlite3_open_simple(case_a, &db_a));
        TEST_STEP(esp_sqlite3_exec(db_a, "CREATE TABLE a(x); INSERT INTO a VALUES(1);"));
        TEST_STEP(esp_sqlite3_open_simple(case_b, &db_b));
        TEST_STEP(esp_sqlite3_exec(db_b, "CREATE TABLE b(x); INSERT INTO b VALUES(2);"));
        TEST_STEP(test_query_int64(db_a, "SELECT COUNT(*) FROM a;", 1));
        TEST_STEP(test_query_int64(db_b, "SELECT COUNT(*) FROM b;", 1));
        TEST_STEP(esp_sqlite3_close(db_a));
        db_a = NULL;
        TEST_STEP(esp_sqlite3_close(db_b));
        db_b = NULL;

        TEST_STEP(esp_sqlite3_open_simple(case_a, &db_a));
        TEST_STEP(test_query_int64(db_a, "SELECT COUNT(*) FROM a;", 1));
        TEST_STEP(esp_sqlite3_close(db_a));
        db_a = NULL;
        test_remove_db(case_a);
        test_remove_db(case_b);
        ESP_LOGI(TEST_TAG, "case-sensitive paths: ok");
    }

    /* ---- 13d. 运行时 temp_store=FILE 必须使用配置的临时目录 ---------- */
#if !defined(SQLITE_OMIT_INTEGRITY_CHECK)
    /* ---- 13d2. 损坏文件必须返回错误而不是崩溃/误报 ok ---------------- */
    {
        const char *bad_path = "/db/corrupt.db";
        test_remove_db(bad_path);
        FILE *fp = fopen(bad_path, "wb");
        TEST_CHECK(fp != NULL, "create corrupt db failed");
        uint8_t garbage[256];
        memset(garbage, 0xAA, sizeof(garbage));
        for (int i = 0; i < 4; i++) {
            TEST_CHECK(fwrite(garbage, 1, sizeof(garbage), fp) == sizeof(garbage),
                       "write corrupt db failed");
        }
        fclose(fp);

        esp_sqlite3_t *bad_db = NULL;
        esp_err_t open_err = esp_sqlite3_open_simple(bad_path, &bad_db);
        if (open_err == ESP_OK) {
            char bad_result[64] = {0};
            esp_err_t bad_err = esp_sqlite3_integrity_check(bad_db, false,
                                                            bad_result, sizeof(bad_result));
            TEST_CHECK(bad_err != ESP_OK, "corrupt db integrity_check unexpectedly passed");
            TEST_STEP(esp_sqlite3_close(bad_db));
            ESP_LOGI(TEST_TAG, "corrupt db: rejected by integrity check (%s)",
                     esp_err_to_name(bad_err));
        } else {
            TEST_CHECK(bad_db == NULL, "corrupt db open returned a handle");
            ESP_LOGI(TEST_TAG, "corrupt db: rejected by open (%s)",
                     esp_err_to_name(open_err));
        }
        test_remove_db(bad_path);
    }
#endif

#if CONFIG_ESP_SQLITE3_THREADSAFE
    /* ---- 13e. 两个任务并发写同一库 ---------------------------------- */
    {
        TEST_STEP(esp_sqlite3_exec(db, "CREATE TABLE conc(v INTEGER);"));
        test_conc_ctx_t c1 = { .path = db_path, .count = 30, .done = false, .err = ESP_FAIL };
        test_conc_ctx_t c2 = { .path = db_path, .count = 30, .done = false, .err = ESP_FAIL };
        TaskHandle_t t1 = NULL;
        TaskHandle_t t2 = NULL;
        BaseType_t ok1 = xTaskCreate(test_conc_task, "sqlite_c1", 6144, &c1, 3, &t1);
        BaseType_t ok2 = xTaskCreate(test_conc_task, "sqlite_c2", 6144, &c2, 3, &t2);
        TEST_CHECK(ok1 == pdPASS && ok2 == pdPASS, "concurrency task create failed");

        int waited_ms = 0;
        while ((!c1.done || !c2.done) && waited_ms < 30000) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited_ms += 100;
        }
        TEST_CHECK(c1.done && c2.done, "concurrency tasks timeout");
        TEST_CHECK(c1.err == ESP_OK, "concurrency task1: %s", esp_err_to_name(c1.err));
        TEST_CHECK(c2.err == ESP_OK, "concurrency task2: %s", esp_err_to_name(c2.err));
        TEST_STEP(test_query_int64(db, "SELECT COUNT(*) FROM conc;", 60));
        ESP_LOGI(TEST_TAG, "concurrent writers: ok (%d ms)", waited_ms);
    }
#endif

#if CONFIG_ESP_SQLITE3_ENABLE_WAL
    /* ---- 13f. PERSIST_WAL 不能受固定表大小限制 ----------------------- */
    {
        char persist_path[6][32];
        for (int i = 0; i < 6; i++) {
            snprintf(persist_path[i], sizeof(persist_path[i]), "/db/persist%d.db", i);
            test_remove_db(persist_path[i]);

            esp_sqlite3_config_t persist_cfg;
            esp_sqlite3_config_init(&persist_cfg);
            persist_cfg.path = persist_path[i];
            persist_cfg.journal_mode = ESP_SQLITE3_JOURNAL_WAL;
            esp_sqlite3_t *persist_db = NULL;
            TEST_STEP(esp_sqlite3_open(&persist_cfg, &persist_db));
            TEST_STEP(esp_sqlite3_exec(persist_db, "CREATE TABLE p(x); INSERT INTO p VALUES(1);"));

            int on = 1;
            int frc = sqlite3_file_control(esp_sqlite3_handle(persist_db), "main",
                                           SQLITE_FCNTL_PERSIST_WAL, &on);
            TEST_CHECK(frc == SQLITE_OK, "PERSIST_WAL[%d] failed: rc=%d", i, frc);
            TEST_STEP(esp_sqlite3_close(persist_db));

            char wal_path[48];
            strncpy(wal_path, persist_path[i], sizeof(wal_path) - 1);
            wal_path[sizeof(wal_path) - 1] = '\0';
            strncat(wal_path, "-wal", sizeof(wal_path) - strlen(wal_path) - 1);
            struct stat st;
            TEST_CHECK(stat(wal_path, &st) == 0, "PERSIST_WAL[%d] deleted %s", i, wal_path);
        }
        for (int i = 0; i < 6; i++) {
            test_remove_db(persist_path[i]);
        }
        ESP_LOGI(TEST_TAG, "PERSIST_WAL x6: ok");
    }
#endif

#ifndef SQLITE_OMIT_TEMPDB
    {
        esp_sqlite3_config_t temp_cfg;
        esp_sqlite3_config_init(&temp_cfg);
        temp_cfg.path = "/db/tempstore.db";
        temp_cfg.temp_store_memory = false;
        esp_sqlite3_t *temp_db = NULL;
        test_remove_db(temp_cfg.path);
        TEST_STEP(esp_sqlite3_open(&temp_cfg, &temp_db));
        TEST_CHECK(sqlite3_temp_directory != NULL &&
                   strcmp(sqlite3_temp_directory, CONFIG_ESP_SQLITE3_TEMP_DIR) == 0,
                   "temp directory not configured: %s",
                   sqlite3_temp_directory != NULL ? sqlite3_temp_directory : "(null)");
        TEST_STEP(test_query_int64(temp_db, "PRAGMA temp_store;", 1));
        TEST_STEP(esp_sqlite3_exec(temp_db,
            "CREATE TEMP TABLE tt(x);"
            "INSERT INTO tt VALUES(1);"
            "INSERT INTO tt SELECT x+1 FROM tt;"
            "INSERT INTO tt SELECT x+1 FROM tt;"));
        TEST_STEP(test_query_int64(temp_db, "SELECT COUNT(*) FROM tt;", 4));
        TEST_STEP(esp_sqlite3_close(temp_db));
        temp_db = NULL;
        test_remove_db(temp_cfg.path);
        ESP_LOGI(TEST_TAG, "temp_store=FILE: ok");
    }
#endif

    /* ---- 14. 内存统计 --------------------------------------------------- */
#if defined(CONFIG_ESP_SQLITE3_MEMSTATUS) && CONFIG_ESP_SQLITE3_MEMSTATUS
    {
        esp_sqlite3_mem_stats_t stats;
        TEST_STEP(esp_sqlite3_get_mem_stats(&stats));
        TEST_CHECK(stats.memory_used > 0, "memory_used=0 with MEMSTATUS enabled");
        TEST_CHECK(stats.malloc_count > 0, "malloc_count=0 with MEMSTATUS enabled");
        ESP_LOGI(TEST_TAG, "mem: used=%lld high=%lld pcache_slots=%lld overflow=%lld blocks=%lld",
                 (long long)stats.memory_used, (long long)stats.memory_highwater,
                 (long long)stats.pagecache_used, (long long)stats.pagecache_overflow,
                 (long long)stats.malloc_count);
    }
#else
    ESP_LOGW(TEST_TAG, "mem stats: skipped (MEMSTATUS disabled)");
#endif

#if !defined(SQLITE_OMIT_WAL)
    /* 留一个 WAL 库，供下一次 MINIMAL/OMIT_WAL 固件验证降级保护 */
    {
        esp_sqlite3_config_t wal_cfg;
        esp_sqlite3_config_init(&wal_cfg);
        wal_cfg.path = TEST_WAL_PROBE;
        wal_cfg.journal_mode = ESP_SQLITE3_JOURNAL_WAL;
        esp_sqlite3_t *wal_db = NULL;
        TEST_STEP(esp_sqlite3_open(&wal_cfg, &wal_db));
        TEST_STEP(esp_sqlite3_exec(wal_db, "CREATE TABLE probe(x);"));
        TEST_STEP(esp_sqlite3_close(wal_db));
        ESP_LOGI(TEST_TAG, "WAL downgrade probe prepared");
    }
#endif

    ESP_LOGI(TEST_TAG, "===== esp_sqlite3 selftest PASSED =====");
    result = ESP_OK;

cleanup:
    if (stmt != NULL) {
        sqlite3_finalize(stmt);
    }
    if (db != NULL) {
        esp_err_t close_err = esp_sqlite3_close(db);
        if (close_err == ESP_OK) {
            db = NULL;
        } else {
            ESP_LOGE(TEST_TAG, "cleanup close failed: %s", esp_err_to_name(close_err));
        }
    }
    if (db == NULL) {
        esp_err_t deinit_err = esp_sqlite3_deinit();
        if (deinit_err != ESP_OK) {
            ESP_LOGE(TEST_TAG, "cleanup deinit failed: %s", esp_err_to_name(deinit_err));
        } else if (result == ESP_OK) {
            /* deinit 后必须能重新 init/open，验证 VFS 锁表/持久 WAL 表已清理 */
            esp_err_t reinit_err = esp_sqlite3_init();
            if (reinit_err != ESP_OK) {
                ESP_LOGE(TEST_TAG, "reinit failed: %s", esp_err_to_name(reinit_err));
                result = reinit_err;
            } else {
                esp_sqlite3_t *reopened = NULL;
                esp_err_t reopen_err = esp_sqlite3_open_simple(db_path, &reopened);
                if (reopen_err != ESP_OK) {
                    ESP_LOGE(TEST_TAG, "reopen after deinit failed: %s",
                             esp_err_to_name(reopen_err));
                    result = reopen_err;
                } else {
                    esp_err_t close_err = esp_sqlite3_close(reopened);
                    if (close_err != ESP_OK) {
                        result = close_err;
                    }
                }
                esp_err_t re_deinit = esp_sqlite3_deinit();
                if (re_deinit != ESP_OK && result == ESP_OK) {
                    result = re_deinit;
                }
                if (result == ESP_OK) {
                    ESP_LOGI(TEST_TAG, "deinit/reinit: ok");
                }
            }
        }
    }
    if (result != ESP_OK) {
        ESP_LOGE(TEST_TAG, "===== esp_sqlite3 selftest FAILED (%s) =====",
                 esp_err_to_name(result));
    }
    test_remove_db(db_path);
    return result;
}

#endif /* CONFIG_ESP_SQLITE3_SELFTEST */
