/**
 * @file main.c
 * @brief esp_sqlite3 压力/掉电/磁盘满/只读挂载测试
 *
 * 阶段 1：8 次随机延迟后 esp_restart()，每次重启校验 WAL/SQLite 完整性；
 * 阶段 2：填满 SPIFFS，确认返回 SQLITE_FULL/IOERR 且清理后仍可写；
 * 阶段 3：只读 VFS 挂载下读取成功、写入返回 EROFS/READONLY；
 * 阶段 4：mmap 已关闭、业务 progress handler 仍能在 VACUUM 后工作。
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_vfs.h"
#include "esp_vfs_ops.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_sqlite3.h"

#define TAG "stress"
#define NVS_NAMESPACE "sqlite_stress"
#define KEY_PHASE "phase"
#define KEY_BOOTS "boots"

#define POWER_DB "/db/power.db"
#define POWER_BATCH 16
#define POWER_CYCLES 8

#define RO_MOUNT "/ro"
#define RO_BACKING "/db"

enum {
    PHASE_INIT = 0,
    PHASE_POWER = 1,
    PHASE_TEST = 2,
    PHASE_DONE = 3,
};

static nvs_handle_t s_nvs;
static const char *s_phase_name[] = { "init", "power", "test", "done" };

static esp_err_t nvs_get_u8_default(const char *key, uint8_t *value)
{
    esp_err_t err = nvs_get_u8(s_nvs, key, value);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *value = 0;
        return ESP_OK;
    }
    return err;
}

static esp_err_t nvs_store(uint8_t phase, uint8_t boots)
{
    esp_err_t err = nvs_set_u8(s_nvs, KEY_PHASE, phase);
    if (err == ESP_OK) {
        err = nvs_set_u8(s_nvs, KEY_BOOTS, boots);
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    return err;
}

static void remove_db_files(const char *path)
{
    char buf[128];
    remove(path);
    snprintf(buf, sizeof(buf), "%s-journal", path);
    remove(buf);
    snprintf(buf, sizeof(buf), "%s-wal", path);
    remove(buf);
    snprintf(buf, sizeof(buf), "%s-shm", path);
    remove(buf);
}

static esp_err_t mount_storage(bool allow_format)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/db",
        .partition_label = "storage",
        .max_files = 16,
        .format_if_mount_failed = allow_format,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed (format=%d): %s", allow_format,
                 esp_err_to_name(err));
        return err;
    }
    size_t total = 0;
    size_t used = 0;
    err = esp_spiffs_info("storage", &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: %u/%u used", (unsigned)used, (unsigned)total);
    }
    return err;
}

static void fill_power_cfg(esp_sqlite3_config_t *cfg)
{
    esp_sqlite3_config_init(cfg);
    cfg->path = POWER_DB;
    cfg->create = true;
    cfg->cache_size_kib = 8;
    cfg->busy_timeout_ms = 5000;
    cfg->temp_store_memory = true;
    cfg->synchronous = ESP_SQLITE3_SYNC_FULL;
#if CONFIG_ESP_SQLITE3_ENABLE_WAL
    cfg->journal_mode = ESP_SQLITE3_JOURNAL_WAL;
#else
    cfg->journal_mode = ESP_SQLITE3_JOURNAL_DELETE;
#endif
}

static esp_err_t validate_power_db(esp_sqlite3_t *db)
{
    char check[128] = {0};
    esp_err_t err = esp_sqlite3_integrity_check(db, false, check, sizeof(check));
    if (err != ESP_OK || strcasecmp(check, "ok") != 0) {
        ESP_LOGE(TAG, "power DB integrity failed: %s (%s)", check,
                 esp_err_to_name(err));
        return ESP_FAIL;
    }
    int64_t count = 0;
    err = esp_sqlite3_query_int64(db, "SELECT COUNT(*) FROM power;", &count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "power count failed: %s", esp_err_to_name(err));
        return err;
    }
    if (count % POWER_BATCH != 0) {
        ESP_LOGE(TAG, "power count=%lld is not a complete batch of %d",
                 (long long)count, POWER_BATCH);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "power DB valid: rows=%lld (batch=%d)", (long long)count,
             POWER_BATCH);
    return ESP_OK;
}

typedef struct {
    volatile bool done;
    esp_err_t err;
} power_writer_t;

static void power_writer_task(void *arg)
{
    power_writer_t *ctx = (power_writer_t *)arg;
    esp_sqlite3_t *db = NULL;
    esp_sqlite3_config_t cfg;
    fill_power_cfg(&cfg);

    esp_err_t err = esp_sqlite3_open(&cfg, &db);
    while (err == ESP_OK && !ctx->done) {
        err = esp_sqlite3_begin(db);
        for (int i = 0; err == ESP_OK && i < POWER_BATCH; i++) {
            err = esp_sqlite3_exec(db,
                "INSERT INTO power(payload) VALUES(randomblob(512));");
        }
        if (err == ESP_OK) {
            err = esp_sqlite3_commit(db);
        } else {
            (void)esp_sqlite3_rollback(db);
        }
    }
    if (db != NULL) {
        esp_err_t close_err = esp_sqlite3_close(db);
        if (err == ESP_OK) {
            err = close_err;
        }
    }
    ctx->err = err;
    ctx->done = true;
    vTaskDelete(NULL);
}

static esp_err_t test_disk_full(void)
{
    const char *path = "/db/full.db";
    remove_db_files(path);

    esp_sqlite3_config_t cfg;
    esp_sqlite3_config_init(&cfg);
    cfg.path = path;
    cfg.cache_size_kib = 4;
    cfg.temp_store_memory = true;
    cfg.synchronous = ESP_SQLITE3_SYNC_FULL;
    cfg.journal_mode = ESP_SQLITE3_JOURNAL_DELETE;

    esp_sqlite3_t *db = NULL;
    esp_err_t err = esp_sqlite3_open(&cfg, &db);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "disk full DB open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_sqlite3_exec(db,
        "CREATE TABLE full(p BLOB);");
    if (err != ESP_OK) {
        esp_sqlite3_close(db);
        return err;
    }

    int writes = 0;
    while (writes < 256 && err == ESP_OK) {
        err = esp_sqlite3_exec(db,
            "INSERT INTO full VALUES(randomblob(16384));");
        writes++;
    }
    int sqlite_rc = esp_sqlite3_errcode(db);
    ESP_LOGI(TAG, "disk full: writes=%d err=%s sqlite_rc=%d",
             writes, esp_err_to_name(err), sqlite_rc);
    if (err == ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS did not report full after %d writes", writes);
        (void)esp_sqlite3_close(db);
        remove_db_files(path);
        return ESP_FAIL;
    }
    if (sqlite_rc != SQLITE_FULL && (sqlite_rc & 0xFF) != SQLITE_IOERR) {
        ESP_LOGE(TAG, "unexpected SQLite error on full disk: rc=%d", sqlite_rc);
        (void)esp_sqlite3_close(db);
        remove_db_files(path);
        return ESP_FAIL;
    }

    esp_err_t close_err = esp_sqlite3_close(db);
    if (close_err != ESP_OK) {
        ESP_LOGE(TAG, "disk full DB close failed: %s", esp_err_to_name(close_err));
    }
    remove_db_files(path);

    /* 删除填满的库后，必须还能重新创建、写入和查询 */
    const char *after = "/db/after_full.db";
    remove_db_files(after);
    err = esp_sqlite3_open_simple(after, &db);
    if (err == ESP_OK) {
        err = esp_sqlite3_exec(db,
            "CREATE TABLE t(v INTEGER); INSERT INTO t VALUES(42);");
    }
    if (err == ESP_OK) {
        int64_t value = 0;
        err = esp_sqlite3_query_int64(db, "SELECT v FROM t;", &value);
        if (err == ESP_OK && value != 42) {
            err = ESP_FAIL;
        }
    }
    if (db != NULL) {
        (void)esp_sqlite3_close(db);
    }
    remove_db_files(after);
    if (err == ESP_OK) {
        size_t total = 0, used = 0;
        (void)esp_spiffs_info("storage", &total, &used);
        ESP_LOGI(TAG, "disk full: recovery ok (free=%u)",
                 (unsigned)(total - used));
    }
    return err;
}

/* -------------------------------------------------------------------------
 * 只读 VFS：把 /ro/xxx 映射到 /db/xxx，VFS 层拒绝所有写打开。
 * ------------------------------------------------------------------------- */

static void ro_map(const char *path, char *out, size_t out_len)
{
    if (path == NULL) {
        path = "";
    }
    if (path[0] == '/') {
        snprintf(out, out_len, RO_BACKING "%s", path);
    } else {
        snprintf(out, out_len, RO_BACKING "/%s", path);
    }
}

static int ro_open(void *ctx, const char *path, int flags, int mode)
{
    (void)ctx;
    if ((flags & O_ACCMODE) != O_RDONLY ||
        (flags & (O_CREAT | O_TRUNC | O_APPEND))) {
        errno = EROFS;
        return -1;
    }
    char full[256];
    ro_map(path, full, sizeof(full));
    return open(full, O_RDONLY, mode);
}

static ssize_t ro_read(void *ctx, int fd, void *dst, size_t size)
{
    (void)ctx;
    return read(fd, dst, size);
}

static ssize_t ro_pread(void *ctx, int fd, void *dst, size_t size, off_t offset)
{
    (void)ctx;
    return pread(fd, dst, size, offset);
}

static off_t ro_lseek(void *ctx, int fd, off_t size, int mode)
{
    (void)ctx;
    return lseek(fd, size, mode);
}

static int ro_close(void *ctx, int fd)
{
    (void)ctx;
    return close(fd);
}

static int ro_fstat(void *ctx, int fd, struct stat *st)
{
    (void)ctx;
    return fstat(fd, st);
}

static int ro_fsync(void *ctx, int fd)
{
    (void)ctx;
    (void)fd;
    return 0;
}

static int ro_fcntl(void *ctx, int fd, int cmd, int arg)
{
    (void)ctx;
    (void)fd;
    (void)cmd;
    (void)arg;
    return 0;
}

static ssize_t ro_write_denied(void *ctx, int fd, const void *data, size_t size)
{
    (void)ctx;
    (void)fd;
    (void)data;
    (void)size;
    errno = EROFS;
    return -1;
}

static ssize_t ro_pwrite_denied(void *ctx, int fd, const void *data,
                                size_t size, off_t offset)
{
    (void)ctx;
    (void)fd;
    (void)data;
    (void)size;
    (void)offset;
    errno = EROFS;
    return -1;
}

static int ro_stat(void *ctx, const char *path, struct stat *st)
{
    (void)ctx;
    char full[256];
    ro_map(path, full, sizeof(full));
    return stat(full, st);
}

static int ro_unlink_denied(void *ctx, const char *path)
{
    (void)ctx;
    (void)path;
    errno = EROFS;
    return -1;
}

static int ro_rename_denied(void *ctx, const char *src, const char *dst)
{
    (void)ctx;
    (void)src;
    (void)dst;
    errno = EROFS;
    return -1;
}

static int ro_link_denied(void *ctx, const char *src, const char *dst)
{
    (void)ctx;
    (void)src;
    (void)dst;
    errno = EROFS;
    return -1;
}

static int ro_mkdir_denied(void *ctx, const char *path, mode_t mode)
{
    (void)ctx;
    (void)path;
    (void)mode;
    errno = EROFS;
    return -1;
}

static int ro_rmdir_denied(void *ctx, const char *path)
{
    (void)ctx;
    (void)path;
    errno = EROFS;
    return -1;
}

static int ro_truncate_denied(void *ctx, const char *path, off_t length)
{
    (void)ctx;
    (void)path;
    (void)length;
    errno = EROFS;
    return -1;
}

static int ro_ftruncate_denied(void *ctx, int fd, off_t length)
{
    (void)ctx;
    (void)fd;
    (void)length;
    errno = EROFS;
    return -1;
}

static int ro_utime_denied(void *ctx, const char *path, const struct utimbuf *times)
{
    (void)ctx;
    (void)path;
    (void)times;
    errno = EROFS;
    return -1;
}

static int ro_access(void *ctx, const char *path, int amode)
{
    (void)ctx;
    char full[256];
    ro_map(path, full, sizeof(full));
    return access(full, amode);
}

static DIR *ro_opendir(void *ctx, const char *path)
{
    (void)ctx;
    char full[256];
    ro_map(path, full, sizeof(full));
    return opendir(full);
}

static struct dirent *ro_readdir(void *ctx, DIR *dir)
{
    (void)ctx;
    return readdir(dir);
}

static int ro_readdir_r(void *ctx, DIR *dir, struct dirent *entry,
                        struct dirent **out)
{
    (void)ctx;
    struct dirent *result = readdir(dir);
    if (out != NULL) {
        *out = result;
    }
    if (result != NULL && entry != NULL) {
        *entry = *result;
    }
    return 0;
}

static long ro_telldir(void *ctx, DIR *dir)
{
    (void)ctx;
    return telldir(dir);
}

static void ro_seekdir(void *ctx, DIR *dir, long offset)
{
    (void)ctx;
    seekdir(dir, offset);
}

static int ro_closedir(void *ctx, DIR *dir)
{
    (void)ctx;
    return closedir(dir);
}

static const esp_vfs_dir_ops_t s_ro_dir = {
    .stat_p = ro_stat,
    .link_p = ro_link_denied,
    .unlink_p = ro_unlink_denied,
    .rename_p = ro_rename_denied,
    .opendir_p = ro_opendir,
    .readdir_p = ro_readdir,
    .readdir_r_p = ro_readdir_r,
    .telldir_p = ro_telldir,
    .seekdir_p = ro_seekdir,
    .closedir_p = ro_closedir,
    .mkdir_p = ro_mkdir_denied,
    .rmdir_p = ro_rmdir_denied,
    .access_p = ro_access,
    .truncate_p = ro_truncate_denied,
    .ftruncate_p = ro_ftruncate_denied,
    .utime_p = ro_utime_denied,
};

static const esp_vfs_fs_ops_t s_ro_ops = {
    .write_p = ro_write_denied,
    .lseek_p = ro_lseek,
    .read_p = ro_read,
    .pread_p = ro_pread,
    .pwrite_p = ro_pwrite_denied,
    .open_p = ro_open,
    .close_p = ro_close,
    .fstat_p = ro_fstat,
    .fcntl_p = ro_fcntl,
    .fsync_p = ro_fsync,
    .dir = &s_ro_dir,
};

static esp_err_t test_readonly_mount(void)
{
    const char *path = "/db/ro_test.db";
    remove_db_files(path);

    esp_sqlite3_t *writer = NULL;
    esp_err_t err = esp_sqlite3_open_simple(path, &writer);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_sqlite3_exec(writer,
        "CREATE TABLE t(v INTEGER);"
        "INSERT INTO t VALUES(1);"
        "INSERT INTO t VALUES(2);"
        "INSERT INTO t VALUES(3);");
    if (err == ESP_OK) {
        err = esp_sqlite3_close(writer);
    } else {
        (void)esp_sqlite3_close(writer);
    }
    if (err != ESP_OK) {
        remove_db_files(path);
        return err;
    }

    err = esp_vfs_register_fs(RO_MOUNT, &s_ro_ops,
                              ESP_VFS_FLAG_CONTEXT_PTR |
                              ESP_VFS_FLAG_STATIC |
                              ESP_VFS_FLAG_READONLY_FS,
                              NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read-only VFS register failed: %s", esp_err_to_name(err));
        remove_db_files(path);
        return err;
    }

    esp_sqlite3_config_t cfg;
    esp_sqlite3_config_init(&cfg);
    cfg.path = "/ro/ro_test.db";
    cfg.create = false;
    cfg.read_only = false;      /* 让 VFS 的 EROFS 触发只读降级 */
    esp_sqlite3_t *ro_db = NULL;
    err = esp_sqlite3_open(&cfg, &ro_db);
    if (err == ESP_OK) {
        int64_t count = 0;
        err = esp_sqlite3_query_int64(ro_db, "SELECT COUNT(*) FROM t;", &count);
        if (err == ESP_OK && count != 3) {
            err = ESP_FAIL;
        }
        if (err == ESP_OK) {
            esp_err_t wr = esp_sqlite3_exec(ro_db,
                "INSERT INTO t VALUES(4);");
            if (wr == ESP_OK) {
                ESP_LOGE(TAG, "read-only VFS accepted a write");
                err = ESP_FAIL;
            } else {
                ESP_LOGI(TAG, "read-only VFS: write rejected (%s)",
                         esp_err_to_name(wr));
            }
        }
        (void)esp_sqlite3_close(ro_db);
    }
    (void)esp_vfs_unregister_fs(RO_MOUNT);
    remove_db_files(path);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "read-only VFS mount: ok");
    }
    return err;
}

static int progress_calls;

static int stress_progress_cb(void *ctx)
{
    (void)ctx;
    progress_calls++;
    return 0;
}

static esp_err_t test_progress_and_mmap(esp_sqlite3_t *db)
{
    int64_t mmap_size = -1;
    esp_err_t err = esp_sqlite3_query_int64(db, "PRAGMA mmap_size;", &mmap_size);
    if (err != ESP_OK || mmap_size != 0) {
        ESP_LOGE(TAG, "mmap_size=%lld (expected 0), err=%s",
                 (long long)mmap_size, esp_err_to_name(err));
        return ESP_FAIL;
    }

    progress_calls = 0;
    err = esp_sqlite3_set_progress_handler(db, 1, stress_progress_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set progress handler failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS progress_test(x);"
        "INSERT INTO progress_test SELECT value FROM ("
        "  WITH RECURSIVE c(value) AS (SELECT 1 UNION ALL SELECT value+1 FROM c WHERE value<100)"
        "  SELECT value FROM c);");
    int before_vacuum = progress_calls;
    if (err == ESP_OK) {
        err = esp_sqlite3_vacuum(db);
    }
    if (err == ESP_OK) {
        err = esp_sqlite3_query_int64(db,
            "SELECT COUNT(*) FROM progress_test;", &mmap_size);
    }
    if (err == ESP_OK && (progress_calls <= before_vacuum || mmap_size != 100)) {
        ESP_LOGE(TAG, "progress handler not preserved: before=%d after=%d",
                 before_vacuum, progress_calls);
        err = ESP_FAIL;
    }
    esp_err_t clear_err = esp_sqlite3_set_progress_handler(db, 0, NULL, NULL);
    if (err == ESP_OK) {
        err = clear_err;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "mmap=0, progress handler: ok (%d calls)", progress_calls);
    }
    return err;
}

static void print_pass_and_halt(void)
{
    ESP_LOGI(TAG, "stress: power-loss/disk-full/read-only/progress PASS");
    ESP_LOGI(TAG, "STRESS RESULT: PASS");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void print_fail_and_halt(const char *where, esp_err_t err)
{
    ESP_LOGE(TAG, "%s failed: %s", where, esp_err_to_name(err));
    ESP_LOGE(TAG, "STRESS RESULT: FAIL (%s: %s)", where, esp_err_to_name(err));
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs));

    uint8_t phase = PHASE_INIT;
    uint8_t boots = 0;
    ESP_ERROR_CHECK(nvs_get_u8_default(KEY_PHASE, &phase));
    ESP_ERROR_CHECK(nvs_get_u8_default(KEY_BOOTS, &boots));
    ESP_LOGI(TAG, "boot: phase=%s(%u) boots=%u",
             phase < 4 ? s_phase_name[phase] : "?", (unsigned)phase,
             (unsigned)boots);

    if (phase == PHASE_DONE) {
        print_pass_and_halt();
    }

    bool first_init = (phase == PHASE_INIT);
    err = mount_storage(first_init);
    if (err != ESP_OK) {
        print_fail_and_halt("mount storage", err);
    }
    err = esp_sqlite3_init();
    if (err != ESP_OK) {
        print_fail_and_halt("sqlite init", err);
    }

    esp_sqlite3_t *power_db = NULL;
    esp_sqlite3_config_t cfg;
    fill_power_cfg(&cfg);
    if (first_init) {
        remove_db_files(POWER_DB);
        cfg.create = true;
    } else {
        cfg.create = false;
    }

    err = esp_sqlite3_open(&cfg, &power_db);
    if (err != ESP_OK) {
        print_fail_and_halt("power DB open", err);
    }
    err = esp_sqlite3_exec(power_db,
        "CREATE TABLE IF NOT EXISTS power("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, payload BLOB);");
    if (err == ESP_OK) {
        err = validate_power_db(power_db);
    }
    if (err != ESP_OK) {
        print_fail_and_halt("power DB validation", err);
    }

    if (first_init) {
        phase = PHASE_POWER;
        boots = 0;
        ESP_ERROR_CHECK(nvs_store(phase, boots));
    }

    if (phase == PHASE_POWER) {
        if (boots >= POWER_CYCLES) {
            phase = PHASE_TEST;
            ESP_ERROR_CHECK(nvs_store(phase, boots));
            ESP_LOGI(TAG, "power-loss cycles complete: %u", (unsigned)boots);
        } else {
            boots++;
            ESP_ERROR_CHECK(nvs_store(phase, boots));
            ESP_LOGI(TAG, "power-loss cycle %u/%u: start writer, reset in %u ms",
                     (unsigned)boots, POWER_CYCLES, (unsigned)(250 + esp_random() % 700));

            power_writer_t writer = { .done = false, .err = ESP_OK };
            BaseType_t ok = xTaskCreate(power_writer_task, "power_writer", 8192,
                                        &writer, 3, NULL);
            if (ok != pdPASS) {
                print_fail_and_halt("power writer task", ESP_ERR_NO_MEM);
            }
            vTaskDelay(pdMS_TO_TICKS(250 + esp_random() % 700));
            esp_restart();
        }
    }

    if (esp_sqlite3_close(power_db) == ESP_OK) {
        power_db = NULL;
    }

    if (phase == PHASE_TEST) {
        err = test_disk_full();
        if (err != ESP_OK) {
            print_fail_and_halt("disk full", err);
        }
        err = test_readonly_mount();
        if (err != ESP_OK) {
            print_fail_and_halt("read-only mount", err);
        }
        esp_sqlite3_t *check_db = NULL;
        err = esp_sqlite3_open_simple(POWER_DB, &check_db);
        if (err != ESP_OK) {
            print_fail_and_halt("reopen power DB", err);
        }
        err = test_progress_and_mmap(check_db);
        if (err == ESP_OK) {
            err = esp_sqlite3_close(check_db);
        }
        if (err != ESP_OK) {
            print_fail_and_halt("progress/mmap", err);
        }
        phase = PHASE_DONE;
        err = nvs_store(phase, boots);
        if (err != ESP_OK) {
            print_fail_and_halt("nvs store done", err);
        }
        print_pass_and_halt();
    }

    print_pass_and_halt();
}
