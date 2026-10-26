/**
 * @file main.c
 * @brief esp_sqlite3 自测工程入口
 *
 * 流程：
 *   1. 挂载 SPIFFS（分区名 storage，挂载点 /db）
 *   2. 调用 esp_sqlite3_run_selftest("/db/selftest.db")
 *   3. 打印结果并保持运行，方便串口查看
 */
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_sqlite3.h"

static const char *TAG = "selftest_main";

static esp_err_t mount_storage(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/db",
        .partition_label = "storage",
        .max_files = 16,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0;
    size_t used = 0;
    ESP_ERROR_CHECK(esp_spiffs_info("storage", &total, &used));
    ESP_LOGI(TAG, "SPIFFS mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "esp_sqlite3 selftest app start");

    esp_err_t err = mount_storage();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage mount failed, cannot run selftest");
    } else {
#if CONFIG_ESP_SQLITE3_SELFTEST
        err = esp_sqlite3_run_selftest("/db/selftest.db");
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SELFTEST RESULT: PASS");
        } else {
            ESP_LOGE(TAG, "SELFTEST RESULT: FAIL (%s)", esp_err_to_name(err));
        }
#else
        /* 体积测量/最小示例：只做一次增删改查，确保核心代码被链接 */
        ESP_ERROR_CHECK(esp_sqlite3_init());
        esp_sqlite3_t *db = NULL;
        err = esp_sqlite3_open_simple("/db/size.db", &db);
        if (err == ESP_OK) {
            esp_sqlite3_exec(db,
                "CREATE TABLE IF NOT EXISTS t(id INTEGER PRIMARY KEY, v TEXT);");
            esp_sqlite3_exec(db, "INSERT INTO t(v) VALUES('x');");
            int64_t count = 0;
            esp_sqlite3_query_int64(db, "SELECT COUNT(*) FROM t;", &count);
            ESP_LOGI(TAG, "CRUD smoke count=%lld", (long long)count);
            esp_sqlite3_close(db);
        } else {
            ESP_LOGE(TAG, "CRUD smoke open failed: %s", esp_err_to_name(err));
        }
#endif
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
