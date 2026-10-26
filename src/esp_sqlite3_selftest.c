/**
 * @file esp_sqlite3_selftest.c
 * @brief 内置自测：覆盖建表、事务、查询、JSON、FTS5、RTREE、shox96、
 *        完整性检查、VACUUM、备份、迁移、WAL checkpoint。
 *
 * 该文件只在 CONFIG_ESP_SQLITE3_SELFTEST=y 时参与编译，
 * 配套工程见 components/esp_sqlite3/test_apps/selftest。
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "sqlite3.h"

#include "esp_sqlite3.h"
#include "esp_sqlite3_internal.h"

#if CONFIG_ESP_SQLITE3_SELFTEST

#define TEST_TAG "esp_sqlite3_test"

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

    TEST_STEP(esp_sqlite3_init());

    /* 打印关键编译选项，方便确认档位是否生效 */
    for (int i = 0; sqlite3_compileoption_get(i) != NULL; i++) {
        ESP_LOGD(TEST_TAG, "compile_option: %s", sqlite3_compileoption_get(i));
    }

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

    /* ---- 13. 内存统计 --------------------------------------------------- */
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

    ESP_LOGI(TEST_TAG, "===== esp_sqlite3 selftest PASSED =====");
    result = ESP_OK;

cleanup:
    if (stmt != NULL) {
        sqlite3_finalize(stmt);
    }
    if (db != NULL) {
        esp_sqlite3_close(db);
    }
    if (result != ESP_OK) {
        ESP_LOGE(TEST_TAG, "===== esp_sqlite3 selftest FAILED (%s) =====",
                 esp_err_to_name(result));
    }
    test_remove_db(db_path);
    return result;
}

#endif /* CONFIG_ESP_SQLITE3_SELFTEST */
