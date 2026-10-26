/**
 * @file esp_sqlite3_mem.c
 * @brief SQLite 内存分配器：优先 PSRAM，小对象留在内部 RAM
 *
 * 为什么不用直接改 sqlite3.c 里的 SQLITE_MALLOC 宏？
 * -------------------------------------------------
 * sqlite3.c 内部会无条件定义 SQLITE_MALLOC，命令行 -D 会造成宏重定义；
 * SQLite 官方推荐的做法是在 sqlite3_initialize() 之前调用
 * sqlite3_config(SQLITE_CONFIG_MALLOC, ...) 注册自定义分配器。
 *
 * 策略：
 *   - 小于 ESP_SQLITE3_PSRAM_THRESHOLD 字节的分配走内部 RAM（访问快）；
 *   - 大对象（页缓存、排序缓冲、WAL 索引）优先 PSRAM；
 *   - PSRAM 不足或未启用时自动回退内部 RAM，保证功能不受影响。
 */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "sqlite3.h"

#include "esp_sqlite3_internal.h"

/** 小于该阈值的分配留在内部 RAM，避免 PSRAM 小对象的额外开销 */
#define ESP_SQLITE3_PSRAM_THRESHOLD (256)

/** 是否允许使用 PSRAM：同时满足 CONFIG_SPIRAM 与 ESP_SQLITE3_PSRAM_PREFER */
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM && \
    defined(CONFIG_ESP_SQLITE3_PSRAM_PREFER) && CONFIG_ESP_SQLITE3_PSRAM_PREFER
#define ESP_SQLITE3_USE_PSRAM 1
#else
#define ESP_SQLITE3_USE_PSRAM 0
#endif

/** PSRAM 是否可用（在注册时探测一次） */
static bool s_psram_available = false;

/* -------------------------------------------------------------------------
 * 分配实现
 * ------------------------------------------------------------------------- */

/** SQLite 默认要求 8 字节对齐；ESP-IDF heap_caps_malloc 只保证 4 字节 */
#define ESP_SQLITE3_MEM_ALIGN 8

/** 分配一块 8 字节对齐的内存，优先 PSRAM，失败回退内部 RAM */
static void *esp_sqlite3_alloc_aligned(size_t n, bool prefer_psram)
{
#if ESP_SQLITE3_USE_PSRAM
    if (prefer_psram && s_psram_available) {
        void *p = heap_caps_aligned_alloc(ESP_SQLITE3_MEM_ALIGN, n,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p != NULL) {
            return p;
        }
    }
#else
    (void)prefer_psram;
#endif

    void *p = heap_caps_aligned_alloc(ESP_SQLITE3_MEM_ALIGN, n,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p == NULL) {
        /* 部分配置下没有单独的内部 heap 标记，最后再放宽一次 */
        p = heap_caps_aligned_alloc(ESP_SQLITE3_MEM_ALIGN, n, MALLOC_CAP_8BIT);
    }
    return p;
}

static void *esp_sqlite3_malloc(int n)
{
    size_t size = (n > 0) ? (size_t)n : 1;
    return esp_sqlite3_alloc_aligned(size, size >= ESP_SQLITE3_PSRAM_THRESHOLD);
}

static void esp_sqlite3_free(void *p)
{
    if (p != NULL) {
        heap_caps_free(p);
    }
}

static void *esp_sqlite3_realloc(void *p, int n)
{
    if (p == NULL) {
        return esp_sqlite3_malloc(n);
    }
    if (n <= 0) {
        esp_sqlite3_free(p);
        return NULL;
    }

    size_t old_size = heap_caps_get_allocated_size(p);
    void *q = esp_sqlite3_malloc(n);
    if (q == NULL) {
        return NULL;
    }
    size_t copy_size = (old_size < (size_t)n) ? old_size : (size_t)n;
    memcpy(q, p, copy_size);
    esp_sqlite3_free(p);
    return q;
}

/**
 * @brief 返回分配的可用大小
 *
 * SQLite 只用它做统计/优化，不要求精确，但必须 >= 请求大小。
 */
static int esp_sqlite3_size(void *p)
{
    if (p == NULL) {
        return 0;
    }
    return (int)heap_caps_get_allocated_size(p);
}

/** SQLite 要求按 8 字节对齐 */
static int esp_sqlite3_roundup(int n)
{
    return (n + 7) & ~7;
}

static int esp_sqlite3_mem_init(void *arg)
{
    (void)arg;
    return SQLITE_OK;
}

static void esp_sqlite3_mem_shutdown(void *arg)
{
    (void)arg;
}

static const sqlite3_mem_methods s_esp_sqlite3_mem_methods = {
    .xMalloc = esp_sqlite3_malloc,
    .xFree = esp_sqlite3_free,
    .xRealloc = esp_sqlite3_realloc,
    .xSize = esp_sqlite3_size,
    .xRoundup = esp_sqlite3_roundup,
    .xInit = esp_sqlite3_mem_init,
    .xShutdown = esp_sqlite3_mem_shutdown,
    .pAppData = NULL,
};

/* -------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------- */

esp_err_t esp_sqlite3_mem_register(void)
{
#if ESP_SQLITE3_USE_PSRAM
    s_psram_available = (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0);
#else
    s_psram_available = false;
#endif

    int rc = sqlite3_config(SQLITE_CONFIG_MALLOC, &s_esp_sqlite3_mem_methods);
    if (rc != SQLITE_OK) {
        /* 最常见原因：在注册前已经调用过 sqlite3_initialize()/sqlite3_open() */
        ESP_SQLITE3_LOGE("register mem methods failed: rc=%d (%s)", rc, sqlite3_errstr(rc));
        return ESP_ERR_INVALID_STATE;
    }

    ESP_SQLITE3_LOGI("memory allocator: %s (psram threshold=%d bytes)",
                    s_psram_available ? "PSRAM preferred" : "internal RAM",
                    ESP_SQLITE3_PSRAM_THRESHOLD);
    return ESP_OK;
}

bool esp_sqlite3_mem_uses_psram(void)
{
    return s_psram_available;
}
