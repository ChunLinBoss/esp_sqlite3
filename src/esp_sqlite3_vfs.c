/**
 * @file esp_sqlite3_vfs.c
 * @brief SQLite 自定义 VFS：ESP-IDF VFS（SPIFFS/FATFS/LittleFS/SD）适配层
 *
 * 与旧社区版（siara-cc / nopnop2002）的关键区别
 * ------------------------------------------------
 * | 能力             | 旧社区版                         | 本实现                     |
 * |------------------|----------------------------------|----------------------------|
 * | 文件锁           | xLock/xUnlock 空实现             | 进程内完整 SHARED/RESERVED/PENDING/EXCLUSIVE 状态机 |
 * | 回滚日志         | 放 RAM 链表，掉电即丢            | 真实 .journal 文件，掉电可恢复 |
 * | 截断             | xTruncate 空实现（VACUUM 失效）  | ftruncate()                |
 * | 同步             | fflush + fsync（可配置）         | fsync()（可关闭）          |
 * | WAL              | SQLITE_OMIT_WAL                  | 完整 xShmMap/xShmLock 支持 |
 * | 错误处理         | 多数返回 OK                      | 返回准确的 SQLITE_IOERR_*  |
 * | 统计             | 无                               | 读写/同步/锁冲突统计       |
 *
 * 路径约定
 * --------
 * - 绝对路径（"/spiffs/app.db"）原样使用；
 * - 相对路径会拼到 CONFIG_ESP_SQLITE3_DEFAULT_DIR 后面；
 * - ":memory:" 由 SQLite 自己处理，不会进入 VFS。
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_random.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sqlite3.h"

#include "esp_sqlite3.h"
#include "esp_sqlite3_internal.h"
#include "esp_sqlite3_vfs.h"

/* -------------------------------------------------------------------------
 * 常量与统计
 * ------------------------------------------------------------------------- */

/** WAL 共享内存锁槽数量（SQLite 固定为 8） */
#define ESP_SQLITE3_VFS_SHM_NLOCK 8

/** 统计计数器（多任务下用 __atomic 累加） */
static esp_sqlite3_vfs_stats_t s_stats;

#define ESP_SQLITE3_STAT_ADD(field, value) \
    __atomic_fetch_add(&s_stats.field, (value), __ATOMIC_RELAXED)
#define ESP_SQLITE3_STAT_SUB(field, value) \
    __atomic_fetch_sub(&s_stats.field, (value), __ATOMIC_RELAXED)

/* -------------------------------------------------------------------------
 * 数据结构
 * ------------------------------------------------------------------------- */

/** 进程内文件锁状态（按规范化路径共享） */
typedef struct esp_sqlite3_vfs_lock {
    char path[CONFIG_ESP_SQLITE3_MAX_PATH];
    int n_shared;                       /**< 当前持有 SHARED 的文件句柄数 */
    struct esp_sqlite3_vfs_file *reserved;       /**< RESERVED 持有者 */
    struct esp_sqlite3_vfs_file *pending;        /**< PENDING 持有者 */
    struct esp_sqlite3_vfs_file *exclusive;      /**< EXCLUSIVE 持有者 */
    int n_refs;                         /**< 打开该数据库的文件句柄数 */
    struct esp_sqlite3_vfs_lock *next;
} esp_sqlite3_vfs_lock_t;

/** WAL 共享内存锁槽 */
typedef struct {
    struct esp_sqlite3_vfs_file *excl;           /**< 独占持有者，NULL 表示空闲 */
    int n_shared;                       /**< 共享持有数 */
} esp_sqlite3_vfs_shm_lock_t;

/** WAL 共享内存区域（每个数据库一份） */
typedef struct esp_sqlite3_vfs_shm {
    char path[CONFIG_ESP_SQLITE3_MAX_PATH];
    int n_ref;                          /**< 引用计数 */
    int n_region;                       /**< 已分配区域数 */
    int region_size;                    /**< 每个区域大小 */
    void **regions;                     /**< 区域指针数组 */
    esp_sqlite3_vfs_shm_lock_t locks[ESP_SQLITE3_VFS_SHM_NLOCK];
    struct esp_sqlite3_vfs_shm *next;
} esp_sqlite3_vfs_shm_t;

/** VFS 文件对象 */
typedef struct esp_sqlite3_vfs_file {
    sqlite3_file base;                  /**< 必须放在第一个成员 */
    int fd;                             /**< ESP-IDF VFS 文件描述符 */
    int open_flags;                     /**< SQLite 打开标志 */
    int lock_level;                     /**< 当前锁级别 */
    bool delete_on_close;               /**< 关闭后删除（临时文件/日志） */
    bool is_wal;                        /**< 是否为 -wal 文件 */
    bool persist_wal;                   /**< SQLITE_FCNTL_PERSIST_WAL */
    char path[CONFIG_ESP_SQLITE3_MAX_PATH]; /**< 规范化后的完整路径 */
    esp_sqlite3_vfs_lock_t *lock;                /**< 文件锁状态 */
    esp_sqlite3_vfs_shm_t *shm;                  /**< WAL 共享内存（仅 -wal 文件） */
} esp_sqlite3_vfs_file_t;

/* -------------------------------------------------------------------------
 * 全局状态
 * ------------------------------------------------------------------------- */

static StaticSemaphore_t s_vfs_mutex_buf;
static SemaphoreHandle_t s_vfs_mutex = NULL;
static esp_sqlite3_vfs_lock_t *s_lock_list = NULL;
static esp_sqlite3_vfs_shm_t *s_shm_list = NULL;
static bool s_registered = false;

/* -------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */

static void esp_sqlite3_vfs_mutex_init_once(void)
{
    if (s_vfs_mutex == NULL) {
        s_vfs_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_vfs_mutex_buf);
    }
}

static inline void esp_sqlite3_vfs_lock_global(void)
{
    esp_sqlite3_vfs_mutex_init_once();
    (void)xSemaphoreTakeRecursive(s_vfs_mutex, portMAX_DELAY);
}

static inline void esp_sqlite3_vfs_unlock_global(void)
{
    (void)xSemaphoreGiveRecursive(s_vfs_mutex);
}

/**
 * @brief 规范化路径：绝对路径原样，相对路径拼默认目录
 */
esp_err_t esp_sqlite3_vfs_make_path(const char *path, char *out, size_t out_len)
{
    if (path == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int n = 0;
    if (path[0] == '/' || strcmp(path, ":memory:") == 0) {
        n = snprintf(out, out_len, "%s", path);
    } else {
        const char *dir = CONFIG_ESP_SQLITE3_DEFAULT_DIR;
        size_t dir_len = strlen(dir);
        /* 避免出现 "//" 或缺少 "/" */
        if (dir_len > 0 && dir[dir_len - 1] == '/') {
            n = snprintf(out, out_len, "%s%s", dir, path);
        } else {
            n = snprintf(out, out_len, "%s/%s", dir, path);
        }
    }

    if (n < 0 || (size_t)n >= out_len) {
        if (out_len > 0) {
            out[0] = '\0';
        }
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/** 生成临时文件路径（不带扩展名） */
static void esp_sqlite3_vfs_make_temp_path(char *out, size_t out_len)
{
    uint32_t a = esp_random();
    uint32_t b = esp_random();
    const char *dir = CONFIG_ESP_SQLITE3_TEMP_DIR;
    size_t dir_len = strlen(dir);

    if (dir_len > 0 && dir[dir_len - 1] == '/') {
        snprintf(out, out_len, "%sesp_sqlite_tmp_%08lx%08lx", dir,
                 (unsigned long)a, (unsigned long)b);
    } else {
        snprintf(out, out_len, "%s/esp_sqlite_tmp_%08lx%08lx", dir,
                 (unsigned long)a, (unsigned long)b);
    }
}

/* -------------------------------------------------------------------------
 * 文件锁表
 * ------------------------------------------------------------------------- */

static esp_sqlite3_vfs_lock_t *esp_sqlite3_lock_find(const char *path)
{
    for (esp_sqlite3_vfs_lock_t *it = s_lock_list; it != NULL; it = it->next) {
        if (strcmp(it->path, path) == 0) {
            return it;
        }
    }
    return NULL;
}

static esp_sqlite3_vfs_lock_t *esp_sqlite3_lock_get(const char *path)
{
    esp_sqlite3_vfs_lock_t *lock = esp_sqlite3_lock_find(path);
    if (lock != NULL) {
        lock->n_refs++;
        return lock;
    }

    lock = sqlite3_malloc64(sizeof(*lock));
    if (lock == NULL) {
        return NULL;
    }
    memset(lock, 0, sizeof(*lock));
    strncpy(lock->path, path, sizeof(lock->path) - 1);
    lock->n_refs = 1;
    lock->next = s_lock_list;
    s_lock_list = lock;
    return lock;
}

static void esp_sqlite3_lock_put(esp_sqlite3_vfs_lock_t *lock)
{
    if (lock == NULL) {
        return;
    }

    lock->n_refs--;
    if (lock->n_refs > 0) {
        return;
    }

    esp_sqlite3_vfs_lock_t **pp = &s_lock_list;
    while (*pp != NULL && *pp != lock) {
        pp = &(*pp)->next;
    }
    if (*pp == lock) {
        *pp = lock->next;
    }
    sqlite3_free(lock);
}

/** 释放某个文件持有的所有锁（不释放 lock 条目本身） */
static void esp_sqlite3_lock_release_all(esp_sqlite3_vfs_file_t *file)
{
    esp_sqlite3_vfs_lock_t *lock = file->lock;
    if (lock == NULL) {
        return;
    }

    if (lock->exclusive == file) {
        lock->exclusive = NULL;
    }
    if (lock->pending == file) {
        lock->pending = NULL;
    }
    if (lock->reserved == file) {
        lock->reserved = NULL;
    }
    if (file->lock_level >= SQLITE_LOCK_SHARED && lock->n_shared > 0) {
        lock->n_shared--;
    }
    file->lock_level = SQLITE_LOCK_NONE;
}

/* -------------------------------------------------------------------------
 * WAL 共享内存表
 * ------------------------------------------------------------------------- */

static esp_sqlite3_vfs_shm_t *esp_sqlite3_shm_find(const char *path)
{
    for (esp_sqlite3_vfs_shm_t *it = s_shm_list; it != NULL; it = it->next) {
        if (strcmp(it->path, path) == 0) {
            return it;
        }
    }
    return NULL;
}

static esp_sqlite3_vfs_shm_t *esp_sqlite3_shm_get(const char *path)
{
    esp_sqlite3_vfs_shm_t *shm = esp_sqlite3_shm_find(path);
    if (shm != NULL) {
        shm->n_ref++;
        return shm;
    }

    shm = sqlite3_malloc64(sizeof(*shm));
    if (shm == NULL) {
        return NULL;
    }
    memset(shm, 0, sizeof(*shm));
    strncpy(shm->path, path, sizeof(shm->path) - 1);
    shm->n_ref = 1;
    shm->next = s_shm_list;
    s_shm_list = shm;
    return shm;
}

static void esp_sqlite3_shm_put(esp_sqlite3_vfs_shm_t *shm)
{
    if (shm == NULL) {
        return;
    }

    shm->n_ref--;
    if (shm->n_ref > 0) {
        return;
    }

    esp_sqlite3_vfs_shm_t **pp = &s_shm_list;
    while (*pp != NULL && *pp != shm) {
        pp = &(*pp)->next;
    }
    if (*pp == shm) {
        *pp = shm->next;
    }

    for (int i = 0; i < shm->n_region; i++) {
        sqlite3_free(shm->regions[i]);
    }
    sqlite3_free(shm->regions);
    sqlite3_free(shm);
}

/* -------------------------------------------------------------------------
 * sqlite3_io_methods 实现
 * ------------------------------------------------------------------------- */

static int esp_sqlite3_vfs_close(sqlite3_file *id)
{
    esp_sqlite3_vfs_file_t *file = (esp_sqlite3_vfs_file_t *)id;
    int rc = SQLITE_OK;

    if (file->fd >= 0) {
        if (file->delete_on_close) {
            /* 先释放锁再删除，避免 Windows/部分 FS 的“打开中不能删除” */
            esp_sqlite3_vfs_lock_global();
            esp_sqlite3_lock_release_all(file);
            esp_sqlite3_vfs_unlock_global();
        }

        if (close(file->fd) != 0) {
            rc = SQLITE_IOERR_CLOSE;
        }
        file->fd = -1;
    }

    if (file->delete_on_close && file->path[0] != '\0') {
        if (unlink(file->path) != 0 && errno != ENOENT) {
            ESP_SQLITE3_LOGW("unlink %s failed: errno=%d", file->path, errno);
        }
    }

    if (file->shm != NULL) {
        esp_sqlite3_vfs_lock_global();
        esp_sqlite3_shm_put(file->shm);
        esp_sqlite3_vfs_unlock_global();
        file->shm = NULL;
    }

    if (file->lock != NULL) {
        esp_sqlite3_vfs_lock_global();
        esp_sqlite3_lock_release_all(file);
        esp_sqlite3_lock_put(file->lock);
        esp_sqlite3_vfs_unlock_global();
        file->lock = NULL;
    }

    ESP_SQLITE3_STAT_SUB(open_files, 1);
    return rc;
}

static int esp_sqlite3_vfs_read(sqlite3_file *id, void *buffer, int amount, sqlite3_int64 offset)
{
    esp_sqlite3_vfs_file_t *file = (esp_sqlite3_vfs_file_t *)id;
    if (amount < 0 || buffer == NULL) {
        return SQLITE_IOERR_READ;
    }
    if (amount == 0) {
        return SQLITE_OK;
    }

    int done = 0;
    while (done < amount) {
        if (lseek(file->fd, (off_t)(offset + done), SEEK_SET) < 0) {
            return SQLITE_IOERR_SEEK;
        }
        ssize_t n = read(file->fd, (uint8_t *)buffer + done, (size_t)(amount - done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SQLITE_IOERR_READ;
        }
        if (n == 0) {
            /* 文件尾部：剩余部分补 0，并告知 SQLite 是短读 */
            memset((uint8_t *)buffer + done, 0, (size_t)(amount - done));
            ESP_SQLITE3_STAT_ADD(read_ops, 1);
            ESP_SQLITE3_STAT_ADD(bytes_read, (uint64_t)done);
            return SQLITE_IOERR_SHORT_READ;
        }
        done += (int)n;
    }

    ESP_SQLITE3_STAT_ADD(read_ops, 1);
    ESP_SQLITE3_STAT_ADD(bytes_read, (uint64_t)done);
    return SQLITE_OK;
}

static int esp_sqlite3_vfs_write(sqlite3_file *id, const void *buffer, int amount, sqlite3_int64 offset)
{
    esp_sqlite3_vfs_file_t *file = (esp_sqlite3_vfs_file_t *)id;
    if (amount < 0 || (buffer == NULL && amount > 0)) {
        return SQLITE_IOERR_WRITE;
    }
    if (amount == 0) {
        return SQLITE_OK;
    }

    int done = 0;
    while (done < amount) {
        if (lseek(file->fd, (off_t)(offset + done), SEEK_SET) < 0) {
            return SQLITE_IOERR_SEEK;
        }
        ssize_t n = write(file->fd, (const uint8_t *)buffer + done, (size_t)(amount - done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SQLITE_IOERR_WRITE;
        }
        if (n == 0) {
            return SQLITE_IOERR_WRITE;
        }
        done += (int)n;
    }

    ESP_SQLITE3_STAT_ADD(write_ops, 1);
    ESP_SQLITE3_STAT_ADD(bytes_written, (uint64_t)done);
    return SQLITE_OK;
}

static int esp_sqlite3_vfs_truncate(sqlite3_file *id, sqlite3_int64 size)
{
    esp_sqlite3_vfs_file_t *file = (esp_sqlite3_vfs_file_t *)id;
    if (ftruncate(file->fd, (off_t)size) != 0) {
        ESP_SQLITE3_LOGE("ftruncate(%s, %lld) failed: errno=%d", file->path,
                        (long long)size, errno);
        return SQLITE_IOERR_TRUNCATE;
    }
    ESP_SQLITE3_STAT_ADD(truncate_ops, 1);
    return SQLITE_OK;
}

static int esp_sqlite3_vfs_sync(sqlite3_file *id, int flags)
{
    (void)flags;
    esp_sqlite3_vfs_file_t *file = (esp_sqlite3_vfs_file_t *)id;

#ifdef SQLITE_NO_SYNC
    (void)file;
    return SQLITE_OK;
#else
    if (fsync(file->fd) != 0) {
        /* 某些文件系统不支持 fsync，返回 ENOSYS 时按“已同步”处理 */
        if (errno == ENOSYS || errno == EINVAL) {
            return SQLITE_OK;
        }
        ESP_SQLITE3_LOGE("fsync(%s) failed: errno=%d", file->path, errno);
        return SQLITE_IOERR_FSYNC;
    }
    ESP_SQLITE3_STAT_ADD(sync_ops, 1);
    return SQLITE_OK;
#endif
}

static int esp_sqlite3_vfs_file_size(sqlite3_file *id, sqlite3_int64 *p_size)
{
    esp_sqlite3_vfs_file_t *file = (esp_sqlite3_vfs_file_t *)id;
    struct stat st;

    if (p_size == NULL) {
        return SQLITE_IOERR;
    }
    if (fstat(file->fd, &st) != 0) {
        return SQLITE_IOERR_FSTAT;
    }
    *p_size = (sqlite3_int64)st.st_size;
    return SQLITE_OK;
}

static int esp_sqlite3_vfs_lock(sqlite3_file *id, int level)
{
    esp_sqlite3_vfs_file_t *file = (esp_sqlite3_vfs_file_t *)id;
    esp_sqlite3_vfs_lock_t *lock = file->lock;

    if (lock == NULL) {
        return SQLITE_IOERR_LOCK;
    }
    if (level <= file->lock_level) {
        return SQLITE_OK;
    }

    int rc = SQLITE_OK;
    esp_sqlite3_vfs_lock_global();

    switch (level) {
    case SQLITE_LOCK_SHARED:
        /* PENDING 锁会阻止新的 SHARED 锁，避免写者饿死 */
        if ((lock->exclusive != NULL && lock->exclusive != file) ||
            (lock->pending != NULL && lock->pending != file)) {
            rc = SQLITE_BUSY;
        } else {
            lock->n_shared++;
        }
        break;

    case SQLITE_LOCK_RESERVED:
        if (file->lock_level < SQLITE_LOCK_SHARED) {
            rc = SQLITE_PROTOCOL;
        } else if (lock->reserved != NULL && lock->reserved != file) {
            rc = SQLITE_BUSY;
        } else {
            lock->reserved = file;
        }
        break;

    case SQLITE_LOCK_PENDING:
        /* 按 SQLite 规定：先 RESERVED，再 PENDING */
        if (file->lock_level < SQLITE_LOCK_RESERVED) {
            rc = SQLITE_PROTOCOL;
        } else if (lock->pending != NULL && lock->pending != file) {
            rc = SQLITE_BUSY;
        } else if (lock->reserved != NULL && lock->reserved != file) {
            rc = SQLITE_BUSY;
        } else {
            lock->pending = file;
        }
        break;

    case SQLITE_LOCK_EXCLUSIVE:
        /* SQLite 允许从 SHARED/RESERVED/PENDING 直接升级到 EXCLUSIVE */
        if (file->lock_level < SQLITE_LOCK_SHARED) {
            rc = SQLITE_PROTOCOL;
        } else if (lock->n_shared > 1) {
            /* 还有其他连接持有 SHARED */
            rc = SQLITE_BUSY;
        } else if (lock->exclusive != NULL && lock->exclusive != file) {
            rc = SQLITE_BUSY;
        } else if (lock->reserved != NULL && lock->reserved != file) {
            rc = SQLITE_BUSY;
        } else {
            lock->exclusive = file;
        }
        break;

    default:
        rc = SQLITE_PROTOCOL;
        break;
    }

    if (rc == SQLITE_OK) {
        file->lock_level = level;
    }
    esp_sqlite3_vfs_unlock_global();

    if (rc == SQLITE_BUSY) {
        ESP_SQLITE3_STAT_ADD(lock_busy, 1);
    }
    return rc;
}

static int esp_sqlite3_vfs_unlock(sqlite3_file *id, int level)
{
    esp_sqlite3_vfs_file_t *file = (esp_sqlite3_vfs_file_t *)id;
    esp_sqlite3_vfs_lock_t *lock = file->lock;

    if (lock == NULL) {
        return SQLITE_OK;
    }
    if (level >= file->lock_level) {
        return SQLITE_OK;
    }

    esp_sqlite3_vfs_lock_global();

    if (file->lock_level == SQLITE_LOCK_EXCLUSIVE && level < SQLITE_LOCK_EXCLUSIVE) {
        if (lock->exclusive == file) {
            lock->exclusive = NULL;
        }
    }
    if (file->lock_level >= SQLITE_LOCK_PENDING && level < SQLITE_LOCK_PENDING) {
        if (lock->pending == file) {
            lock->pending = NULL;
        }
    }
    if (file->lock_level >= SQLITE_LOCK_RESERVED && level < SQLITE_LOCK_RESERVED) {
        if (lock->reserved == file) {
            lock->reserved = NULL;
        }
    }
    if (file->lock_level >= SQLITE_LOCK_SHARED && level < SQLITE_LOCK_SHARED) {
        if (lock->n_shared > 0) {
            lock->n_shared--;
        }
    }

    file->lock_level = level;
    esp_sqlite3_vfs_unlock_global();
    return SQLITE_OK;
}

static int esp_sqlite3_vfs_check_reserved_lock(sqlite3_file *id, int *p_reserved)
{
    esp_sqlite3_vfs_file_t *file = (esp_sqlite3_vfs_file_t *)id;
    if (p_reserved == NULL) {
        return SQLITE_IOERR;
    }

    esp_sqlite3_vfs_lock_global();
    /* 与 POSIX 语义保持一致：自己持有的 RESERVED 不算“别人持有” */
    *p_reserved = (file->lock != NULL && file->lock->reserved != NULL &&
                   file->lock->reserved != file) ? 1 : 0;
    esp_sqlite3_vfs_unlock_global();
    return SQLITE_OK;
}

static int esp_sqlite3_vfs_file_control(sqlite3_file *id, int op, void *arg)
{
    esp_sqlite3_vfs_file_t *file = (esp_sqlite3_vfs_file_t *)id;

    switch (op) {
    case SQLITE_FCNTL_SIZE_HINT:
        /*
         * 不预分配：SPIFFS/FATFS 上预分配会立刻占用空间，且对写入速度帮助有限。
         * 返回 OK 让 SQLite 继续。
         */
        return SQLITE_OK;

    case SQLITE_FCNTL_CHUNK_SIZE:
        return SQLITE_OK;

    case SQLITE_FCNTL_PERSIST_WAL:
        if (arg != NULL) {
            file->persist_wal = (*(int *)arg != 0);
        }
        return SQLITE_OK;

    case SQLITE_FCNTL_TEMPFILENAME: {
        if (arg == NULL) {
            return SQLITE_NOTFOUND;
        }
        char path[CONFIG_ESP_SQLITE3_MAX_PATH];
        esp_sqlite3_vfs_make_temp_path(path, sizeof(path));
        char **p_out = (char **)arg;
        *p_out = sqlite3_mprintf("%s", path);
        return (*p_out != NULL) ? SQLITE_OK : SQLITE_NOMEM;
    }

    case SQLITE_FCNTL_VFSNAME: {
        if (arg == NULL) {
            return SQLITE_NOTFOUND;
        }
        char **p_out = (char **)arg;
        *p_out = sqlite3_mprintf("%s", CONFIG_ESP_SQLITE3_VFS_NAME);
        return (*p_out != NULL) ? SQLITE_OK : SQLITE_NOMEM;
    }

    case SQLITE_FCNTL_HAS_MOVED: {
        if (arg == NULL) {
            return SQLITE_NOTFOUND;
        }
        struct stat st;
        *(int *)arg = (stat(file->path, &st) != 0) ? 1 : 0;
        return SQLITE_OK;
    }

    case SQLITE_FCNTL_LOCK_TIMEOUT:
        /* 本 VFS 不内部阻塞，交给 SQLite 的 busy handler 重试 */
        return SQLITE_NOTFOUND;

    case SQLITE_FCNTL_SYNC_OMITTED:
        return SQLITE_OK;

    default:
        return SQLITE_NOTFOUND;
    }
}

static int esp_sqlite3_vfs_sector_size(sqlite3_file *id)
{
    (void)id;
    int sector = CONFIG_ESP_SQLITE3_SECTOR_SIZE;
    /* SQLite 要求扇区大小是 2 的幂；配置错误时回退到 4096 */
    if (sector < 512 || (sector & (sector - 1)) != 0) {
        sector = 4096;
    }
    return sector;
}

static int esp_sqlite3_vfs_device_characteristics(sqlite3_file *id)
{
    (void)id;
    int flags = 0;
#ifdef CONFIG_ESP_SQLITE3_IOCAP_POWERSAFE_OVERWRITE
#if CONFIG_ESP_SQLITE3_IOCAP_POWERSAFE_OVERWRITE
    flags |= SQLITE_IOCAP_POWERSAFE_OVERWRITE;
#endif
#endif
    return flags;
}

/* -------------------------------------------------------------------------
 * WAL 共享内存
 * ------------------------------------------------------------------------- */

static int esp_sqlite3_vfs_shm_map(sqlite3_file *id, int iPg, int pgsz, int bExtend, void volatile **pp)
{
    esp_sqlite3_vfs_file_t *file = (esp_sqlite3_vfs_file_t *)id;
    if (pp == NULL) {
        return SQLITE_IOERR;
    }
    *pp = NULL;

    esp_sqlite3_vfs_lock_global();

    if (file->shm == NULL) {
        file->shm = esp_sqlite3_shm_get(file->path);
        if (file->shm == NULL) {
            esp_sqlite3_vfs_unlock_global();
            return SQLITE_NOMEM;
        }
    }

    esp_sqlite3_vfs_shm_t *shm = file->shm;

    if (iPg < 0) {
        esp_sqlite3_vfs_unlock_global();
        return SQLITE_IOERR;
    }

    if (iPg >= shm->n_region) {
        if (!bExtend) {
            esp_sqlite3_vfs_unlock_global();
            return SQLITE_OK;
        }

        int new_n = iPg + 1;
        void **new_regions = sqlite3_realloc64(shm->regions, (sqlite3_uint64)new_n * sizeof(void *));
        if (new_regions == NULL) {
            esp_sqlite3_vfs_unlock_global();
            return SQLITE_NOMEM;
        }
        shm->regions = new_regions;

        for (int i = shm->n_region; i < new_n; i++) {
            shm->regions[i] = sqlite3_malloc64((sqlite3_uint64)pgsz);
            if (shm->regions[i] == NULL) {
                /* 回滚本次扩展，保持结构一致 */
                for (int j = shm->n_region; j < i; j++) {
                    sqlite3_free(shm->regions[j]);
                    shm->regions[j] = NULL;
                }
                esp_sqlite3_vfs_unlock_global();
                return SQLITE_NOMEM;
            }
            memset(shm->regions[i], 0, (size_t)pgsz);
        }
        shm->n_region = new_n;
        shm->region_size = pgsz;
    }

    *pp = shm->regions[iPg];
    esp_sqlite3_vfs_unlock_global();
    return SQLITE_OK;
}

static int esp_sqlite3_vfs_shm_lock(sqlite3_file *id, int offset, int n, int flags)
{
    esp_sqlite3_vfs_file_t *file = (esp_sqlite3_vfs_file_t *)id;
    if (offset < 0 || n <= 0 || (offset + n) > ESP_SQLITE3_VFS_SHM_NLOCK) {
        return SQLITE_IOERR;
    }
    if (file->shm == NULL) {
        return SQLITE_IOERR;
    }

    int rc = SQLITE_OK;
    bool is_unlock = (flags & SQLITE_SHM_UNLOCK) != 0;
    bool is_exclusive = (flags & SQLITE_SHM_EXCLUSIVE) != 0;

    esp_sqlite3_vfs_lock_global();

    for (int i = offset; i < offset + n; i++) {
        esp_sqlite3_vfs_shm_lock_t *slot = &file->shm->locks[i];

        if (is_unlock) {
            if (is_exclusive) {
                if (slot->excl == file) {
                    slot->excl = NULL;
                }
            } else {
                if (slot->n_shared > 0) {
                    slot->n_shared--;
                }
            }
        } else if (is_exclusive) {
            if ((slot->excl != NULL && slot->excl != file) || slot->n_shared > 0) {
                rc = SQLITE_BUSY;
                break;
            }
            slot->excl = file;
        } else {
            if (slot->excl != NULL && slot->excl != file) {
                rc = SQLITE_BUSY;
                break;
            }
            slot->n_shared++;
        }
    }

    /* 批量加锁失败时，回滚已经成功的部分，避免状态泄漏 */
    if (rc != SQLITE_OK && !is_unlock) {
        for (int i = offset; i < offset + n; i++) {
            esp_sqlite3_vfs_shm_lock_t *slot = &file->shm->locks[i];
            if (is_exclusive) {
                if (slot->excl == file) {
                    slot->excl = NULL;
                }
            } else {
                if (slot->n_shared > 0) {
                    slot->n_shared--;
                }
            }
        }
    }

    esp_sqlite3_vfs_unlock_global();

    if (rc == SQLITE_BUSY) {
        ESP_SQLITE3_STAT_ADD(lock_busy, 1);
    }
    return rc;
}

static void esp_sqlite3_vfs_shm_barrier(sqlite3_file *id)
{
    (void)id;
    __sync_synchronize();
}

static int esp_sqlite3_vfs_shm_unmap(sqlite3_file *id, int delete_flag)
{
    (void)delete_flag; /* 我们不创建真实的 -shm 文件 */
    esp_sqlite3_vfs_file_t *file = (esp_sqlite3_vfs_file_t *)id;

    if (file->shm != NULL) {
        esp_sqlite3_vfs_lock_global();
        esp_sqlite3_shm_put(file->shm);
        esp_sqlite3_vfs_unlock_global();
        file->shm = NULL;
    }
    return SQLITE_OK;
}

/* -------------------------------------------------------------------------
 * sqlite3_vfs 实现
 * ------------------------------------------------------------------------- */

/** 文件方法表：必须是静态生命周期，SQLite 会长期持有该指针 */
static const sqlite3_io_methods s_esp_sqlite3_io_methods = {
    .iVersion = 2,
    .xClose = esp_sqlite3_vfs_close,
    .xRead = esp_sqlite3_vfs_read,
    .xWrite = esp_sqlite3_vfs_write,
    .xTruncate = esp_sqlite3_vfs_truncate,
    .xSync = esp_sqlite3_vfs_sync,
    .xFileSize = esp_sqlite3_vfs_file_size,
    .xLock = esp_sqlite3_vfs_lock,
    .xUnlock = esp_sqlite3_vfs_unlock,
    .xCheckReservedLock = esp_sqlite3_vfs_check_reserved_lock,
    .xFileControl = esp_sqlite3_vfs_file_control,
    .xSectorSize = esp_sqlite3_vfs_sector_size,
    .xDeviceCharacteristics = esp_sqlite3_vfs_device_characteristics,
    .xShmMap = esp_sqlite3_vfs_shm_map,
    .xShmLock = esp_sqlite3_vfs_shm_lock,
    .xShmBarrier = esp_sqlite3_vfs_shm_barrier,
    .xShmUnmap = esp_sqlite3_vfs_shm_unmap,
    .xFetch = NULL,
    .xUnfetch = NULL,
};

static int esp_sqlite3_vfs_open(sqlite3_vfs *vfs, const char *path, sqlite3_file *file,
                       int flags, int *out_flags)
{
    (void)vfs;

    if (file == NULL) {
        return SQLITE_IOERR;
    }

    /* SQLite 要求 VFS 必须清零整个 szOsFile，否则未初始化字段会带脏数据 */
    memset(file, 0, sizeof(esp_sqlite3_vfs_file_t));
    esp_sqlite3_vfs_file_t *f = (esp_sqlite3_vfs_file_t *)file;
    f->fd = -1;

    if (path == NULL) {
        return SQLITE_CANTOPEN;
    }

    char full[CONFIG_ESP_SQLITE3_MAX_PATH];
    if (path[0] == '\0') {
        /* 临时库路径为空：生成一个唯一的绝对路径 */
        esp_sqlite3_vfs_make_temp_path(full, sizeof(full));
    } else {
        esp_err_t err = esp_sqlite3_vfs_make_path(path, full, sizeof(full));
        if (err != ESP_OK) {
            ESP_SQLITE3_LOGE("path too long: %s", path);
            return SQLITE_CANTOPEN;
        }
    }

    int oflags = 0;
    if (flags & SQLITE_OPEN_READONLY) {
        oflags = O_RDONLY;
    } else {
        oflags = O_RDWR;
    }
    if (flags & SQLITE_OPEN_CREATE) {
        oflags |= O_CREAT;
    }
    /*
     * 不映射 SQLITE_OPEN_EXCLUSIVE 到 O_EXCL：
     * SPIFFS/LittleFS 对 O_EXCL 的支持并不一致，而 ESP-IDF 是单进程环境，
     * 由 SQLite 自身的文件锁已经足够。
     */

    f->fd = open(full, oflags, 0666);
    if (f->fd < 0) {
        ESP_SQLITE3_LOGD("open(%s, 0x%x) failed: errno=%d", full, oflags, errno);
        return SQLITE_CANTOPEN;
    }

    strncpy(f->path, full, sizeof(f->path) - 1);
    f->open_flags = flags;
    f->delete_on_close = (flags & SQLITE_OPEN_DELETEONCLOSE) != 0;
    f->is_wal = (flags & SQLITE_OPEN_WAL) != 0;
    f->lock_level = SQLITE_LOCK_NONE;

    esp_sqlite3_vfs_lock_global();
    f->lock = esp_sqlite3_lock_get(full);
    esp_sqlite3_vfs_unlock_global();
    if (f->lock == NULL) {
        close(f->fd);
        f->fd = -1;
        return SQLITE_NOMEM;
    }

    f->base.pMethods = &s_esp_sqlite3_io_methods;

    if (out_flags != NULL) {
        *out_flags = flags;
    }

    ESP_SQLITE3_STAT_ADD(open_files, 1);
    ESP_SQLITE3_STAT_ADD(total_opens, 1);
    ESP_SQLITE3_LOGD("open %s flags=0x%x fd=%d", f->path, flags, f->fd);
    return SQLITE_OK;
}

static int esp_sqlite3_vfs_delete(sqlite3_vfs *vfs, const char *path, int sync_dir)
{
    (void)vfs;
    (void)sync_dir;

    char full[CONFIG_ESP_SQLITE3_MAX_PATH];
    if (esp_sqlite3_vfs_make_path(path, full, sizeof(full)) != ESP_OK) {
        return SQLITE_IOERR_DELETE;
    }

    if (unlink(full) != 0) {
        if (errno == ENOENT) {
            return SQLITE_IOERR_DELETE_NOENT;
        }
        ESP_SQLITE3_LOGW("unlink(%s) failed: errno=%d", full, errno);
        return SQLITE_IOERR_DELETE;
    }
    return SQLITE_OK;
}

static int esp_sqlite3_vfs_access(sqlite3_vfs *vfs, const char *path, int flags, int *p_res_out)
{
    (void)vfs;
    if (p_res_out == NULL) {
        return SQLITE_IOERR;
    }
    *p_res_out = 0;

    char full[CONFIG_ESP_SQLITE3_MAX_PATH];
    if (esp_sqlite3_vfs_make_path(path, full, sizeof(full)) != ESP_OK) {
        return SQLITE_OK;
    }

    struct stat st;
    if (stat(full, &st) != 0) {
        *p_res_out = 0;
        return SQLITE_OK;
    }

    /*
     * 只要文件存在就返回 1。SPIFFS/FATFS 的 st_mode 并不总是可靠地反映
     * 只读挂载；真正的写权限由后续 open(O_RDWR) 是否成功来判定。
     */
    (void)flags;
    *p_res_out = 1;
    return SQLITE_OK;
}

static int esp_sqlite3_vfs_full_pathname(sqlite3_vfs *vfs, const char *path, int n_out, char *z_out)
{
    (void)vfs;
    if (z_out == NULL || n_out <= 0) {
        return SQLITE_CANTOPEN;
    }

    char full[CONFIG_ESP_SQLITE3_MAX_PATH];
    if (esp_sqlite3_vfs_make_path(path, full, sizeof(full)) != ESP_OK) {
        return SQLITE_CANTOPEN;
    }

    int len = (int)strlen(full);
    if (len >= n_out) {
        return SQLITE_CANTOPEN;
    }
    memcpy(z_out, full, (size_t)len + 1);
    return SQLITE_OK;
}

static void *esp_sqlite3_vfs_dl_open(sqlite3_vfs *vfs, const char *path)
{
    (void)vfs;
    (void)path;
    return NULL;
}

static void esp_sqlite3_vfs_dl_error(sqlite3_vfs *vfs, int n_byte, char *z_err_msg)
{
    (void)vfs;
    if (z_err_msg != NULL && n_byte > 0) {
        snprintf(z_err_msg, (size_t)n_byte, "dynamic loading not supported");
    }
}

static void (*esp_sqlite3_vfs_dl_sym(sqlite3_vfs *vfs, void *handle, const char *symbol))(void)
{
    (void)vfs;
    (void)handle;
    (void)symbol;
    return NULL;
}

static void esp_sqlite3_vfs_dl_close(sqlite3_vfs *vfs, void *handle)
{
    (void)vfs;
    (void)handle;
}

static int esp_sqlite3_vfs_randomness(sqlite3_vfs *vfs, int n_byte, char *z_out)
{
    (void)vfs;
    if (n_byte <= 0) {
        return 0;
    }
    if (z_out == NULL) {
        return 0;
    }

    int done = 0;
    while (done < n_byte) {
        uint32_t r = esp_random();
        int n = (n_byte - done) < (int)sizeof(r) ? (n_byte - done) : (int)sizeof(r);
        memcpy(z_out + done, &r, (size_t)n);
        done += n;
    }
    return n_byte;
}

static int esp_sqlite3_vfs_sleep(sqlite3_vfs *vfs, int microseconds)
{
    (void)vfs;
    if (microseconds <= 0) {
        return SQLITE_OK;
    }

    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        TickType_t ticks = pdMS_TO_TICKS((microseconds + 999) / 1000);
        if (ticks == 0) {
            ticks = 1;
        }
        vTaskDelay(ticks);
    } else {
        esp_rom_delay_us((uint32_t)microseconds);
    }
    return SQLITE_OK;
}

static int esp_sqlite3_vfs_current_time(sqlite3_vfs *vfs, double *p_time)
{
    (void)vfs;
    if (p_time == NULL) {
        return SQLITE_ERROR;
    }

    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return SQLITE_ERROR;
    }

    /* Julian day：1970-01-01 00:00:00 UTC = 2440587.5 */
    *p_time = (double)tv.tv_sec / 86400.0 + 2440587.5 +
              (double)tv.tv_usec / 86400000000.0;
    return SQLITE_OK;
}

static int esp_sqlite3_vfs_current_time_int64(sqlite3_vfs *vfs, sqlite3_int64 *p_time)
{
    (void)vfs;
    if (p_time == NULL) {
        return SQLITE_ERROR;
    }

    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return SQLITE_ERROR;
    }

    /* 2440587.5 * 86400000 = 210866760000000 */
    *p_time = (sqlite3_int64)tv.tv_sec * 1000 + (sqlite3_int64)(tv.tv_usec / 1000) +
              210866760000000LL;
    return SQLITE_OK;
}

static int esp_sqlite3_vfs_get_last_error(sqlite3_vfs *vfs, int n_byte, char *z_out)
{
    (void)vfs;
    int err = errno;
    if (z_out != NULL && n_byte > 0) {
        snprintf(z_out, (size_t)n_byte, "%s", strerror(err));
    }
    return err;
}

/* -------------------------------------------------------------------------
 * VFS 实例与注册
 * ------------------------------------------------------------------------- */

static sqlite3_vfs s_esp_sqlite3_vfs = {
    .iVersion = 3,
    .szOsFile = sizeof(esp_sqlite3_vfs_file_t),
    .mxPathname = CONFIG_ESP_SQLITE3_MAX_PATH,
    .pNext = NULL,
    .zName = CONFIG_ESP_SQLITE3_VFS_NAME,
    .pAppData = NULL,
    .xOpen = esp_sqlite3_vfs_open,
    .xDelete = esp_sqlite3_vfs_delete,
    .xAccess = esp_sqlite3_vfs_access,
    .xFullPathname = esp_sqlite3_vfs_full_pathname,
    .xDlOpen = esp_sqlite3_vfs_dl_open,
    .xDlError = esp_sqlite3_vfs_dl_error,
    .xDlSym = esp_sqlite3_vfs_dl_sym,
    .xDlClose = esp_sqlite3_vfs_dl_close,
    .xRandomness = esp_sqlite3_vfs_randomness,
    .xSleep = esp_sqlite3_vfs_sleep,
    .xCurrentTime = esp_sqlite3_vfs_current_time,
    .xGetLastError = esp_sqlite3_vfs_get_last_error,
    .xCurrentTimeInt64 = esp_sqlite3_vfs_current_time_int64,
    .xSetSystemCall = NULL,
    .xGetSystemCall = NULL,
    .xNextSystemCall = NULL,
};

sqlite3_vfs *esp_sqlite3_vfs_get(void)
{
    return &s_esp_sqlite3_vfs;
}

esp_err_t esp_sqlite3_vfs_register(bool make_default)
{
    esp_sqlite3_vfs_mutex_init_once();

    if (s_registered) {
        return ESP_OK;
    }

    int rc = sqlite3_vfs_register(&s_esp_sqlite3_vfs, make_default ? 1 : 0);
    if (rc != SQLITE_OK) {
        ESP_SQLITE3_LOGE("sqlite3_vfs_register failed: rc=%d", rc);
        return ESP_FAIL;
    }
    s_registered = true;
    ESP_SQLITE3_LOGI("VFS registered: %s (mxPathname=%d)", s_esp_sqlite3_vfs.zName, s_esp_sqlite3_vfs.mxPathname);
    return ESP_OK;
}

void esp_sqlite3_vfs_get_stats(esp_sqlite3_vfs_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    /* 32/64 位字段分别原子读取，允许轻微不一致 */
    out->open_files = __atomic_load_n(&s_stats.open_files, __ATOMIC_RELAXED);
    out->total_opens = __atomic_load_n(&s_stats.total_opens, __ATOMIC_RELAXED);
    out->read_ops = __atomic_load_n(&s_stats.read_ops, __ATOMIC_RELAXED);
    out->write_ops = __atomic_load_n(&s_stats.write_ops, __ATOMIC_RELAXED);
    out->sync_ops = __atomic_load_n(&s_stats.sync_ops, __ATOMIC_RELAXED);
    out->truncate_ops = __atomic_load_n(&s_stats.truncate_ops, __ATOMIC_RELAXED);
    out->lock_busy = __atomic_load_n(&s_stats.lock_busy, __ATOMIC_RELAXED);
    out->bytes_read = __atomic_load_n(&s_stats.bytes_read, __ATOMIC_RELAXED);
    out->bytes_written = __atomic_load_n(&s_stats.bytes_written, __ATOMIC_RELAXED);
}

void esp_sqlite3_vfs_reset_stats(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
}

/* -------------------------------------------------------------------------
 * SQLITE_OS_OTHER 入口
 *
 * 当 SQLITE_OS_OTHER=1 时，SQLite 不再编译 os_unix.c/os_win.c，
 * 而是在 sqlite3_initialize() 中调用我们提供的 sqlite3_os_init()。
 * ------------------------------------------------------------------------- */

int sqlite3_os_init(void)
{
    esp_err_t err = esp_sqlite3_vfs_register(true);
    return (err == ESP_OK) ? SQLITE_OK : SQLITE_ERROR;
}

int sqlite3_os_end(void)
{
    /* sqlite3_shutdown() 之后再次 initialize 时需要重新注册 VFS */
    s_registered = false;
    return SQLITE_OK;
}
