/**
 * @file esp_sqlite3_mutex.c
 * @brief SQLite 互斥量实现（FreeRTOS 递归信号量）
 *
 * SQLite 在 SQLITE_OS_OTHER=1 且非 Unix/Windows 平台时默认使用 NOOP 互斥量，
 * 因此多任务环境下必须通过 sqlite3_config(SQLITE_CONFIG_MUTEX) 注册我们自己的
 * 实现。这里全部使用递归信号量：
 *   - SQLITE_MUTEX_FAST 也按递归实现（语义更安全，代价很小）；
 *   - 静态互斥量在 xMutexInit 中一次性创建，避免运行时并发创建；
 *   - 动态互斥量使用 SQLite 自己的分配器，便于统计和 PSRAM 管理。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sqlite3.h"

#include "esp_sqlite3_internal.h"

/** SQLite 静态互斥量 ID 范围：2..13（见 sqlite3.c 内部定义） */
#define ESP_SQLITE3_STATIC_MUTEX_MIN 2
#define ESP_SQLITE3_STATIC_MUTEX_MAX 13
#define ESP_SQLITE3_STATIC_MUTEX_NUM (ESP_SQLITE3_STATIC_MUTEX_MAX + 1)

/** 互斥量包装 */
typedef struct {
    SemaphoreHandle_t sem;   /**< FreeRTOS 递归信号量 */
    bool is_static;          /**< 静态互斥量不允许 free */
} esp_sqlite3_mutex_t;

/** 静态互斥量池（索引即 SQLite 的 mutex id） */
static esp_sqlite3_mutex_t s_static_mutexes[ESP_SQLITE3_STATIC_MUTEX_NUM];

/** 保护动态互斥量创建/销毁的注册锁 */
static SemaphoreHandle_t s_registry_mutex = NULL;

static bool s_mutex_initialized = false;

/* -------------------------------------------------------------------------
 * sqlite3_mutex_methods 实现
 * ------------------------------------------------------------------------- */

static int esp_sqlite3_mutex_init(void)
{
    if (s_mutex_initialized) {
        return SQLITE_OK;
    }

    s_registry_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_registry_mutex == NULL) {
        return SQLITE_NOMEM;
    }

    for (int id = ESP_SQLITE3_STATIC_MUTEX_MIN; id <= ESP_SQLITE3_STATIC_MUTEX_MAX; id++) {
        s_static_mutexes[id].sem = xSemaphoreCreateRecursiveMutex();
        s_static_mutexes[id].is_static = true;
        if (s_static_mutexes[id].sem == NULL) {
            /* 创建失败时把已创建的回收，保持状态一致 */
            for (int j = ESP_SQLITE3_STATIC_MUTEX_MIN; j < id; j++) {
                vSemaphoreDelete(s_static_mutexes[j].sem);
                s_static_mutexes[j].sem = NULL;
            }
            vSemaphoreDelete(s_registry_mutex);
            s_registry_mutex = NULL;
            return SQLITE_NOMEM;
        }
    }

    s_mutex_initialized = true;
    return SQLITE_OK;
}

static int esp_sqlite3_mutex_end(void)
{
    if (!s_mutex_initialized) {
        return SQLITE_OK;
    }

    for (int id = ESP_SQLITE3_STATIC_MUTEX_MIN; id <= ESP_SQLITE3_STATIC_MUTEX_MAX; id++) {
        if (s_static_mutexes[id].sem != NULL) {
            vSemaphoreDelete(s_static_mutexes[id].sem);
            s_static_mutexes[id].sem = NULL;
        }
    }

    if (s_registry_mutex != NULL) {
        vSemaphoreDelete(s_registry_mutex);
        s_registry_mutex = NULL;
    }

    s_mutex_initialized = false;
    return SQLITE_OK;
}

static sqlite3_mutex *esp_sqlite3_mutex_alloc(int id)
{
    if (id >= ESP_SQLITE3_STATIC_MUTEX_MIN && id <= ESP_SQLITE3_STATIC_MUTEX_MAX) {
        return (sqlite3_mutex *)&s_static_mutexes[id];
    }

    if (id != SQLITE_MUTEX_FAST && id != SQLITE_MUTEX_RECURSIVE) {
        return NULL;
    }

    esp_sqlite3_mutex_t *m = sqlite3_malloc64(sizeof(*m));
    if (m == NULL) {
        return NULL;
    }

    m->sem = xSemaphoreCreateRecursiveMutex();
    m->is_static = false;
    if (m->sem == NULL) {
        sqlite3_free(m);
        return NULL;
    }

    return (sqlite3_mutex *)m;
}

static void esp_sqlite3_mutex_free(sqlite3_mutex *p)
{
    if (p == NULL) {
        return;
    }

    esp_sqlite3_mutex_t *m = (esp_sqlite3_mutex_t *)p;
    if (m->is_static) {
        /* 静态互斥量由 xMutexEnd 统一销毁 */
        return;
    }

    if (m->sem != NULL) {
        vSemaphoreDelete(m->sem);
    }
    sqlite3_free(m);
}

static void esp_sqlite3_mutex_enter(sqlite3_mutex *p)
{
    esp_sqlite3_mutex_t *m = (esp_sqlite3_mutex_t *)p;
    if (m != NULL && m->sem != NULL) {
        /* portMAX_DELAY：与 SQLite 的 busy handler 配合，不在这里做超时 */
        (void)xSemaphoreTakeRecursive(m->sem, portMAX_DELAY);
    }
}

static int esp_sqlite3_mutex_try(sqlite3_mutex *p)
{
    esp_sqlite3_mutex_t *m = (esp_sqlite3_mutex_t *)p;
    if (m == NULL || m->sem == NULL) {
        return SQLITE_OK;
    }
    return (xSemaphoreTakeRecursive(m->sem, 0) == pdTRUE) ? SQLITE_OK : SQLITE_BUSY;
}

static void esp_sqlite3_mutex_leave(sqlite3_mutex *p)
{
    esp_sqlite3_mutex_t *m = (esp_sqlite3_mutex_t *)p;
    if (m != NULL && m->sem != NULL) {
        (void)xSemaphoreGiveRecursive(m->sem);
    }
}

static int esp_sqlite3_mutex_held(sqlite3_mutex *p)
{
    esp_sqlite3_mutex_t *m = (esp_sqlite3_mutex_t *)p;
    if (m == NULL || m->sem == NULL) {
        return 1;
    }
    return (xSemaphoreGetMutexHolder(m->sem) == xTaskGetCurrentTaskHandle()) ? 1 : 0;
}

static int esp_sqlite3_mutex_notheld(sqlite3_mutex *p)
{
    return esp_sqlite3_mutex_held(p) ? 0 : 1;
}

static const sqlite3_mutex_methods s_esp_sqlite3_mutex_methods __attribute__((unused)) = {
    .xMutexInit = esp_sqlite3_mutex_init,
    .xMutexEnd = esp_sqlite3_mutex_end,
    .xMutexAlloc = esp_sqlite3_mutex_alloc,
    .xMutexFree = esp_sqlite3_mutex_free,
    .xMutexEnter = esp_sqlite3_mutex_enter,
    .xMutexTry = esp_sqlite3_mutex_try,
    .xMutexLeave = esp_sqlite3_mutex_leave,
    .xMutexHeld = esp_sqlite3_mutex_held,
    .xMutexNotheld = esp_sqlite3_mutex_notheld,
};

/* -------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------- */

esp_err_t esp_sqlite3_mutex_register(void)
{
#if SQLITE_THREADSAFE
    int rc = sqlite3_config(SQLITE_CONFIG_MUTEX, &s_esp_sqlite3_mutex_methods);
    if (rc != SQLITE_OK) {
        ESP_SQLITE3_LOGE("register mutex methods failed: rc=%d (%s)", rc, sqlite3_errstr(rc));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
#else
    /* SQLITE_THREADSAFE=0 时 SQLite 使用 MUTEX_OMIT，不能再注册互斥方法 */
    return ESP_OK;
#endif
}
