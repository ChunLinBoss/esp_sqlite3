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
    char path[CONFIG_ESP_SQLITE3_MAX_PATH + 16];
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
    char path[CONFIG_ESP_SQLITE3_MAX_PATH + 16];
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
    char path[CONFIG_ESP_SQLITE3_MAX_PATH + 16]; /**< 规范化后的完整路径 */
    esp_sqlite3_vfs_lock_t *lock;                /**< 文件锁状态 */
    esp_sqlite3_vfs_shm_t *shm;                  /**< WAL 共享内存（仅 -wal 文件） */
    uint8_t shared_locks[ESP_SQLITE3_VFS_SHM_NLOCK]; /**< 本文件持有的 WAL read lock 槽位 */
} esp_sqlite3_vfs_file_t;

/* -------------------------------------------------------------------------
 * 全局状态
 * ------------------------------------------------------------------------- */

static StaticSemaphore_t s_vfs_mutex_buf;
static SemaphoreHandle_t s_vfs_mutex = NULL;
static esp_sqlite3_vfs_lock_t *s_lock_list = NULL;
static esp_sqlite3_vfs_shm_t *s_shm_list = NULL;
static bool s_registered = false;

/* SQLITE_FCNTL_PERSIST_WAL 标记的数据库路径（不含 -wal 后缀） */
#define ESP_SQLITE3_VFS_MAX_PERSIST_WAL 4
static char s_persist_wal[ESP_SQLITE3_VFS_MAX_PERSIST_WAL][CONFIG_ESP_SQLITE3_MAX_PATH + 16];
static int s_persist_wal_count = 0;

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

static bool esp_sqlite3_vfs_persist_wal_contains(const char *path)
{
    for (int i = 0; i < s_persist_wal_count; i++) {
        if (strcmp(s_persist_wal[i], path) == 0) {
            return true;
        }
    }
    return false;
}

static void esp_sqlite3_vfs_persist_wal_set(const char *path, bool persist)
{
    int found = -1;
    for (int i = 0; i < s_persist_wal_count; i++) {
        if (strcmp(s_persist_wal[i], path) == 0) {
            found = i;
            break;
        }
    }

    if (persist) {
        if (found >= 0) {
            return;
        }
        if (s_persist_wal_count >= ESP_SQLITE3_VFS_MAX_PERSIST_WAL) {
            return;
        }
        strncpy(s_persist_wal[s_persist_wal_count], path,
                sizeof(s_persist_wal[0]) - 1);
        s_persist_wal[s_persist_wal_count][sizeof(s_persist_wal[0]) - 1] = '\0';
        s_persist_wal_count++;
    } else if (found >= 0) {
        s_persist_wal[found][0] = '\0';
        for (int i = found; i + 1 < s_persist_wal_count; i++) {
            memcpy(s_persist_wal[i], s_persist_wal[i + 1], sizeof(s_persist_wal[0]));
        }
        s_persist_wal_count--;
    }
}

/**
 * @brief 词法规范化路径：合并重复 '/'，去掉 '.'，消解 '..'
 *
 * 不做大小写折叠：SPIFFS/LittleFS 区分大小写，FATFS 不区分；
 * 调用者应保持同一数据库使用同一种大小写写法。
 */
static esp_err_t esp_sqlite3_vfs_normalize_path(const char *in, char *out, size_t out_len)
{
    if (in == NULL || out == NULL || out_len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(in, ":memory:") == 0) {
        if (strlen(in) >= out_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        strcpy(out, in);
        return ESP_OK;
    }

    bool absolute = (in[0] == '/');
    size_t olen = 0;
    if (absolute) {
        out[olen++] = '/';
    }

    const char *p = in;
    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        const char *seg = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        size_t seg_len = (size_t)(p - seg);
        if (seg_len == 0 || (seg_len == 1 && seg[0] == '.')) {
            continue;
        }

        bool pop = false;
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (olen > (absolute ? 1u : 0u)) {
                pop = true;
            } else if (absolute) {
                continue; /* 根目录之上的 '..' 直接忽略 */
            }
        }

        if (pop) {
            if (olen > 0 && out[olen - 1] == '/') {
                olen--;
            }
            while (olen > (absolute ? 1u : 0u) && out[olen - 1] != '/') {
                olen--;
            }
            if (olen > (absolute ? 1u : 0u) && out[olen - 1] == '/') {
                olen--;
            }
            continue;
        }

        if (olen > 0 && out[olen - 1] != '/') {
            if (olen + 1 >= out_len) {
                return ESP_ERR_INVALID_SIZE;
            }
            out[olen++] = '/';
        }
        if (olen + seg_len >= out_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out + olen, seg, seg_len);
        olen += seg_len;
    }

    if (olen == 0) {
        out[olen++] = absolute ? '/' : '.';
    }
    out[olen] = '\0';
    return ESP_OK;
}

/**
 * @brief 规范化路径：绝对路径原样，相对路径拼默认目录
 */
esp_err_t esp_sqlite3_vfs_make_path(const char *path, char *out, size_t out_len)
{
    if (path == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char tmp[CONFIG_ESP_SQLITE3_MAX_PATH + 16];
    int n = 0;
    if (path[0] == '/' || strcmp(path, ":memory:") == 0) {
        n = snprintf(tmp, sizeof(tmp), "%s", path);
    } else {
        const char *dir = CONFIG_ESP_SQLITE3_DEFAULT_DIR;
        size_t dir_len = strlen(dir);
        /* 避免出现 "//" 或缺少 "/" */
        if (dir_len > 0 && dir[dir_len - 1] == '/') {
            n = snprintf(tmp, sizeof(tmp), "%s%s", dir, path);
        } else {
            n = snprintf(tmp, sizeof(tmp), "%s/%s", dir, path);
        }
    }

    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        if (out_len > 0) {
            out[0] = '\0';
        }
        return ESP_ERR_INVALID_SIZE;
    }
    return esp_sqlite3_vfs_normalize_path(tmp, out, out_len);
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

    /*
     * SPIFFS 的 lseek 不允许越过文件末尾（会返回 EIO），而 SQLite 在
     * 打开新库/读页尾时经常会“读文件末尾之后”的位置。VFS 规范要求
     * 这种情况返回 SHORT_READ，并把未读部分清零，不能把 EIO 直接上抛。
     */
    struct stat st;
    if (fstat(file->fd, &st) != 0) {
        ESP_SQLITE3_LOGE("fstat read %s failed: errno=%d", file->path, errno);
        return SQLITE_IOERR_FSTAT;
    }
    sqlite3_int64 file_size = (sqlite3_int64)st.st_size;
    if (offset >= file_size) {
        memset(buffer, 0, (size_t)amount);
        ESP_SQLITE3_STAT_ADD(read_ops, 1);
        return SQLITE_IOERR_SHORT_READ;
    }

    int done = 0;
    int want = amount;
    sqlite3_int64 avail = file_size - offset;
    if (avail < (sqlite3_int64)want) {
        want = (int)avail;
    }

    while (done < want) {
        if (lseek(file->fd, (off_t)(offset + done), SEEK_SET) < 0) {
            ESP_SQLITE3_LOGE("lseek read %s off=%lld failed: errno=%d",
                            file->path, (long long)(offset + done), errno);
            return SQLITE_IOERR_SEEK;
        }
        ssize_t n = read(file->fd, (uint8_t *)buffer + done, (size_t)(want - done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_SQLITE3_LOGE("read(%s, %d) failed: errno=%d", file->path, want - done, errno);
            return SQLITE_IOERR_READ;
        }
        if (n == 0) {
            break;
        }
        done += (int)n;
    }

    ESP_SQLITE3_STAT_ADD(read_ops, 1);
    ESP_SQLITE3_STAT_ADD(bytes_read, (uint64_t)done);
    if (done < amount) {
        /* 文件尾部：剩余部分补 0，并告知 SQLite 是短读 */
        memset((uint8_t *)buffer + done, 0, (size_t)(amount - done));
        return SQLITE_IOERR_SHORT_READ;
    }
    return SQLITE_OK;
}

/** 把 POSIX errno 映射成 SQLite I/O 错误码 */
static int esp_sqlite3_vfs_errno_to_ioerr(int err, int fallback)
{
    switch (err) {
    case ENOSPC:
        return SQLITE_FULL;
    case EROFS:
    case EPERM:
    case EACCES:
        return SQLITE_READONLY;
    default:
        return fallback;
    }
}

/**
 * @brief 把文件扩展到指定长度（用 0 填充）
 *
 * SPIFFS 的 lseek 不允许越过 EOF，所以 SQLite 写稀疏区域时必须先显式
 * 补零扩展。正常数据库/ WAL 写入是顺序的，不会频繁走到这里。
 */
static int esp_sqlite3_vfs_extend_to(esp_sqlite3_vfs_file_t *file, sqlite3_int64 target)
{
    struct stat st;
    if (fstat(file->fd, &st) != 0) {
        return SQLITE_IOERR_FSTAT;
    }
    sqlite3_int64 current = (sqlite3_int64)st.st_size;
    if (target <= current) {
        return SQLITE_OK;
    }
    if (lseek(file->fd, 0, SEEK_END) < 0) {
        return SQLITE_IOERR_SEEK;
    }

    uint8_t zeros[256] = {0};
    sqlite3_int64 remain = target - current;
    while (remain > 0) {
        size_t chunk = (remain > (sqlite3_int64)sizeof(zeros)) ? sizeof(zeros) : (size_t)remain;
        ssize_t n = write(file->fd, zeros, chunk);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return esp_sqlite3_vfs_errno_to_ioerr(errno, SQLITE_IOERR_WRITE);
        }
        if (n == 0) {
            return SQLITE_IOERR_WRITE;
        }
        remain -= n;
    }
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

    int rc = esp_sqlite3_vfs_extend_to(file, offset);
    if (rc != SQLITE_OK) {
        ESP_SQLITE3_LOGE("extend %s to %lld failed: rc=%d", file->path,
                        (long long)offset, rc);
        return rc;
    }

    int done = 0;
    while (done < amount) {
        if (lseek(file->fd, (off_t)(offset + done), SEEK_SET) < 0) {
            ESP_SQLITE3_LOGE("lseek write %s off=%lld failed: errno=%d",
                            file->path, (long long)(offset + done), errno);
            return SQLITE_IOERR_SEEK;
        }
        ssize_t n = write(file->fd, (const uint8_t *)buffer + done, (size_t)(amount - done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            int ioerr = esp_sqlite3_vfs_errno_to_ioerr(errno, SQLITE_IOERR_WRITE);
            ESP_SQLITE3_LOGE("write(%s, %d) failed: errno=%d rc=%d",
                            file->path, amount, errno, ioerr);
            return ioerr;
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
    if (size < 0) {
        return SQLITE_IOERR_TRUNCATE;
    }
    int rc = esp_sqlite3_vfs_extend_to(file, size);
    if (rc != SQLITE_OK) {
        return rc;
    }
    if (ftruncate(file->fd, (off_t)size) != 0) {
        int ioerr = esp_sqlite3_vfs_errno_to_ioerr(errno, SQLITE_IOERR_TRUNCATE);
        ESP_SQLITE3_LOGE("ftruncate(%s, %lld) failed: errno=%d rc=%d", file->path,
                        (long long)size, errno, ioerr);
        return ioerr;
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
            esp_sqlite3_vfs_lock_global();
            esp_sqlite3_vfs_persist_wal_set(file->path, file->persist_wal);
            esp_sqlite3_vfs_unlock_global();
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

    if (iPg < 0 || pgsz <= 0) {
        esp_sqlite3_vfs_unlock_global();
        return SQLITE_IOERR;
    }
    if (shm->region_size != 0 && shm->region_size != pgsz) {
        esp_sqlite3_vfs_unlock_global();
        return SQLITE_IOERR_SHMSIZE;
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
    uint8_t acquired_shared[ESP_SQLITE3_VFS_SHM_NLOCK] = {0};
    uint8_t acquired_excl[ESP_SQLITE3_VFS_SHM_NLOCK] = {0};
    int failed_at = offset + n;

    esp_sqlite3_vfs_lock_global();

    for (int i = offset; i < offset + n; i++) {
        esp_sqlite3_vfs_shm_lock_t *slot = &file->shm->locks[i];

        if (is_unlock) {
            if (is_exclusive) {
                if (slot->excl == file) {
                    slot->excl = NULL;
                }
            } else if (file->shared_locks[i] > 0) {
                file->shared_locks[i]--;
                if (slot->n_shared > 0) {
                    slot->n_shared--;
                }
            }
        } else if (is_exclusive) {
            if ((slot->excl != NULL && slot->excl != file) ||
                slot->n_shared > 0 || file->shared_locks[i] > 0) {
                rc = SQLITE_BUSY;
                failed_at = i;
                break;
            }
            if (slot->excl != file) {
                acquired_excl[i - offset] = 1;
            }
            slot->excl = file;
        } else {
            if (slot->excl != NULL && slot->excl != file) {
                rc = SQLITE_BUSY;
                failed_at = i;
                break;
            }
            slot->n_shared++;
            file->shared_locks[i]++;
            acquired_shared[i - offset] = 1;
        }
    }

    /* 批量加锁失败时，只回滚本次真正获取的槽位，不能释放别人的读锁 */
    if (rc != SQLITE_OK && !is_unlock) {
        for (int i = offset; i < failed_at; i++) {
            esp_sqlite3_vfs_shm_lock_t *slot = &file->shm->locks[i];
            int idx = i - offset;
            if (acquired_shared[idx] && slot->n_shared > 0) {
                slot->n_shared--;
                if (file->shared_locks[i] > 0) {
                    file->shared_locks[i]--;
                }
            }
            if (acquired_excl[idx] && slot->excl == file) {
                slot->excl = NULL;
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
        /* 关闭连接/重新映射前，释放本文件仍持有的所有 WAL 锁 */
        for (int i = 0; i < ESP_SQLITE3_VFS_SHM_NLOCK; i++) {
            esp_sqlite3_vfs_shm_lock_t *slot = &file->shm->locks[i];
            if (file->shared_locks[i] > 0) {
                if (slot->n_shared >= file->shared_locks[i]) {
                    slot->n_shared -= file->shared_locks[i];
                } else {
                    slot->n_shared = 0;
                }
                file->shared_locks[i] = 0;
            }
            if (slot->excl == file) {
                slot->excl = NULL;
            }
        }
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

    /*
     * SQLite 允许 path 为 NULL：此时必须生成唯一的临时文件名。
     * SQLITE_OPEN_DELETEONCLOSE 的临时库、子日志和外部排序都依赖这个契约。
     */
    char full[CONFIG_ESP_SQLITE3_MAX_PATH + 16];
    if (path == NULL) {
        esp_sqlite3_vfs_make_temp_path(full, sizeof(full));
        if (full[0] == '\0') {
            return SQLITE_CANTOPEN;
        }
    } else if (path[0] == '\0') {
        esp_sqlite3_vfs_make_temp_path(full, sizeof(full));
        if (full[0] == '\0') {
            return SQLITE_CANTOPEN;
        }
    } else {
        esp_err_t err = esp_sqlite3_vfs_make_path(path, full, sizeof(full));
        if (err != ESP_OK) {
            ESP_SQLITE3_LOGE("path too long: %s", path);
            return SQLITE_CANTOPEN;
        }
    }

    int oflags = (flags & SQLITE_OPEN_READONLY) ? O_RDONLY : O_RDWR;
    bool wants_create = (flags & SQLITE_OPEN_CREATE) != 0;
    bool wants_exclusive = (flags & SQLITE_OPEN_EXCLUSIVE) != 0;
    int fd = -1;
    int saved_errno = 0;

    /*
     * 关键：ESP-IDF 6.1 的 SPIFFS 只在 O_CREAT|O_EXCL 或 O_CREAT|O_TRUNC
     * 时才真正创建文件，单独的 O_CREAT 会被丢弃。这里先尝试 O_EXCL 创建，
     * 已存在时再按普通模式打开；不能给已有数据库加 O_TRUNC，否则会清空数据。
     */
    if (wants_create) {
        fd = open(full, oflags | O_CREAT | O_EXCL, 0666);
        if (fd < 0) {
            saved_errno = errno;
            if (!wants_exclusive && saved_errno == EEXIST) {
                fd = open(full, oflags);
                if (fd < 0) {
                    saved_errno = errno;
                }
            }
        }
    } else {
        fd = open(full, oflags);
        if (fd < 0) {
            saved_errno = errno;
        }
    }

    int actual_flags = flags;
    if (fd < 0 && !(flags & SQLITE_OPEN_READONLY) &&
        (saved_errno == EACCES || saved_errno == EROFS || saved_errno == EPERM)) {
        /* 只读挂载/写保护介质：按 SQLite VFS 约定降级为只读打开 */
        fd = open(full, O_RDONLY);
        if (fd >= 0) {
            actual_flags = (flags & ~(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) |
                           SQLITE_OPEN_READONLY;
            ESP_SQLITE3_LOGW("open %s read-write failed, fallback to read-only", full);
        } else {
            saved_errno = errno;
        }
    }

    if (fd < 0) {
        ESP_SQLITE3_LOGD("open(%s, 0x%x) failed: errno=%d", full, oflags, saved_errno);
        if (saved_errno == ENOSPC) {
            return SQLITE_FULL;
        }
        if (saved_errno == EACCES || saved_errno == EROFS || saved_errno == EPERM) {
            return SQLITE_READONLY;
        }
        return SQLITE_CANTOPEN;
    }
    f->fd = fd;

    strncpy(f->path, full, sizeof(f->path) - 1);
    f->open_flags = actual_flags;
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
        *out_flags = actual_flags;
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

    char full[CONFIG_ESP_SQLITE3_MAX_PATH + 16];
    if (esp_sqlite3_vfs_make_path(path, full, sizeof(full)) != ESP_OK) {
        return SQLITE_IOERR_DELETE;
    }

    size_t full_len = strlen(full);
    if (full_len > 4 && strcmp(full + full_len - 4, "-wal") == 0) {
        esp_sqlite3_vfs_lock_global();
        char base[CONFIG_ESP_SQLITE3_MAX_PATH + 16];
        size_t base_len = full_len - 4;
        if (base_len < sizeof(base)) {
            memcpy(base, full, base_len);
            base[base_len] = '\0';
            if (esp_sqlite3_vfs_persist_wal_contains(base)) {
                esp_sqlite3_vfs_unlock_global();
                return SQLITE_OK;
            }
        }
        esp_sqlite3_vfs_unlock_global();
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

    char full[CONFIG_ESP_SQLITE3_MAX_PATH + 16];
    if (esp_sqlite3_vfs_make_path(path, full, sizeof(full)) != ESP_OK) {
        return SQLITE_CANTOPEN;
    }

    struct stat st;
    if (stat(full, &st) != 0) {
        *p_res_out = 0;
        return SQLITE_OK;
    }

    /*
     * SPIFFS/FATFS 的 st_mode 不一定可靠；用 access() 判断实际权限。
     * SQLite 会把 READWRITE 失败当成只读数据库处理，避免后续 xOpen
     * 反复以读写模式失败。
     */
    if (flags == SQLITE_ACCESS_READWRITE) {
        *p_res_out = (access(full, R_OK | W_OK) == 0) ? 1 : 0;
    } else if (flags == SQLITE_ACCESS_READ) {
        *p_res_out = (access(full, R_OK) == 0) ? 1 : 0;
    } else {
        *p_res_out = 1;
    }
    return SQLITE_OK;
}

static int esp_sqlite3_vfs_full_pathname(sqlite3_vfs *vfs, const char *path, int n_out, char *z_out)
{
    (void)vfs;
    if (z_out == NULL || n_out <= 0) {
        return SQLITE_CANTOPEN;
    }

    char full[CONFIG_ESP_SQLITE3_MAX_PATH + 16];
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

    /*
     * sqlite3_vfs_register() 内部会触发 sqlite3_initialize()。如果用户在
     * esp_sqlite3_init() 之前调用本函数，MALLOC/MUTEX 还没配置，后续 init
     * 会永久失败。初始化过程中由 sqlite3_os_init() 调用时放行。
     */
    if (!esp_sqlite3_is_initialized() && !esp_sqlite3_is_initializing()) {
        ESP_SQLITE3_LOGE("esp_sqlite3_init() must be called before vfs_register()");
        return ESP_ERR_INVALID_STATE;
    }

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
    /*
     * SQLite 在 sqlite3_initialize() 内回调这里。sqlite3_vfs_register()
     * 会递归进入 sqlite3_initialize()，但 SQLite 用 inProgress 保护了这段；
     * 前提是动态 mutex 不能使用 sqlite3_malloc（见 esp_sqlite3_mutex.c）。
     */
    esp_err_t err = esp_sqlite3_vfs_register(true);
    return (err == ESP_OK) ? SQLITE_OK : SQLITE_ERROR;
}

int sqlite3_os_end(void)
{
    /* sqlite3_shutdown() 之后再次 initialize 时需要重新注册 VFS */
    s_registered = false;
    return SQLITE_OK;
}
