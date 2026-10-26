/**
 * @file esp_sqlite3_ext.c
 * @brief SQLite 扩展函数
 *
 * 1. 兼容旧社区版：shox96_0_2c() / shox96_0_2d()
 *    - 旧版 siara-cc/nopnop2002 通过 sqlite3_auto_extension 注册了这两个
 *      压缩/解压函数，已有数据库/业务 SQL 可能依赖它们，因此原样保留；
 *    - 算法实现位于 shox96_0_2.c（Apache-2.0，作者 Arundale R.）。
 *
 * 2. 新增调试函数：
 *    - esp_sqlite_version()     : SQLite 版本字符串
 *    - esp_sqlite_uptime_ms()   : 系统启动后的毫秒数（esp_timer）
 *    - esp_sqlite_random()      : 32 位硬件随机数
 *    - esp_sqlite_mem_used()    : 当前 SQLite 堆内存占用
 */
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "sqlite3.h"

#include "esp_sqlite3_internal.h"
#include "shox96_0_2.h"

/* -------------------------------------------------------------------------
 * 变长整数（与旧版 esp32.c 中的实现保持一致）
 * ------------------------------------------------------------------------- */

static int esp_sqlite3_encode_unsigned_varint(uint8_t *buffer, uint64_t value)
{
    int encoded = 0;
    do {
        uint8_t next_byte = (uint8_t)(value & 0x7F);
        value >>= 7;
        if (value) {
            next_byte |= 0x80;
        }
        buffer[encoded++] = next_byte;
    } while (value);
    return encoded;
}

static esp_err_t esp_sqlite3_decode_unsigned_varint(const uint8_t *data, int max_bytes,
                                                    uint64_t *out_value, int *decoded_bytes)
{
    if (data == NULL || out_value == NULL || decoded_bytes == NULL || max_bytes <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t value = 0;
    int shift = 0;
    int i = 0;
    while (i < max_bytes && i < 10) {
        uint8_t byte = data[i++];
        if (shift >= 64 || (shift == 63 && (byte & 0x7E) != 0)) {
            return ESP_ERR_INVALID_ARG;
        }
        value |= (uint64_t)(byte & 0x7F) << shift;
        shift += 7;
        if ((byte & 0x80) == 0) {
            *out_value = value;
            *decoded_bytes = i;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

/* -------------------------------------------------------------------------
 * shox96 压缩 / 解压 SQL 函数
 * ------------------------------------------------------------------------- */

static void esp_sqlite3_shox96_compress(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    if (argc != 1) {
        sqlite3_result_error(context, "shox96_0_2c expects 1 argument", -1);
        return;
    }

    int n_in = sqlite3_value_bytes(argv[0]);
    const unsigned char *in = (const unsigned char *)sqlite3_value_blob(argv[0]);
    if (in == NULL && n_in > 0) {
        sqlite3_result_error_nomem(context);
        return;
    }

    uint8_t varint[10];
    int varint_len = esp_sqlite3_encode_unsigned_varint(varint, (uint64_t)n_in);

    /* 与旧版一致的上界：13 + len + len/1000，再预留 varint 长度 */
    if (n_in < 0 || n_in > INT_MAX - 64) {
        sqlite3_result_error(context, "shox96 input too large", -1);
        return;
    }
    int out_cap = 13 + n_in + (n_in + 999) / 1000 + varint_len;
    unsigned char *out = sqlite3_malloc64((sqlite3_uint64)out_cap);
    if (out == NULL) {
        sqlite3_result_error_nomem(context);
        return;
    }

    memcpy(out, varint, (size_t)varint_len);
    int n_out = shox96_0_2_compress((const char *)in, n_in,
                                    (char *)out + varint_len,
                                    out_cap - varint_len, NULL);
    if (n_out < 0 || n_out > out_cap - varint_len) {
        sqlite3_free(out);
        sqlite3_result_error(context, "shox96 compress failed", -1);
        return;
    }

    sqlite3_result_blob(context, out, n_out + varint_len, sqlite3_free);
}

static void esp_sqlite3_shox96_decompress(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    if (argc != 1) {
        sqlite3_result_error(context, "shox96_0_2d expects 1 argument", -1);
        return;
    }
    if (sqlite3_value_type(argv[0]) != SQLITE_BLOB) {
        sqlite3_result_error(context, "shox96_0_2d expects a BLOB", -1);
        return;
    }

    int n_in = sqlite3_value_bytes(argv[0]);
    const unsigned char *in = (const unsigned char *)sqlite3_value_blob(argv[0]);
    if (in == NULL || n_in < 1) {
        sqlite3_result_error(context, "shox96 blob too short", -1);
        return;
    }

    int varint_len = 0;
    uint64_t out_len64 = 0;
    esp_err_t verr = esp_sqlite3_decode_unsigned_varint(in, n_in, &out_len64, &varint_len);
    if (verr != ESP_OK || varint_len > n_in ||
        (varint_len == n_in && out_len64 != 0) ||
        out_len64 > (uint64_t)INT32_MAX) {
        sqlite3_result_error(context, "invalid shox96 header", -1);
        return;
    }

    unsigned char *out = sqlite3_malloc64(out_len64 > 0 ? out_len64 : 1);
    if (out == NULL) {
        sqlite3_result_error_nomem(context);
        return;
    }

    int out_cap = (out_len64 > 0) ? (int)out_len64 : 1;
    int n_out = shox96_0_2_decompress((const char *)in + varint_len,
                                      n_in - varint_len, (char *)out, out_cap, NULL);
    if (n_out < 0 || n_out != (int)out_len64) {
        sqlite3_free(out);
        sqlite3_result_error(context, "shox96 decompress failed", -1);
        return;
    }

    sqlite3_result_blob(context, out, n_out, sqlite3_free);
}

/* -------------------------------------------------------------------------
 * esp_sqlite_* 辅助函数
 * ------------------------------------------------------------------------- */

static void esp_sqlite_version_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    (void)argc;
    (void)argv;
    sqlite3_result_text(context, sqlite3_libversion(), -1, SQLITE_STATIC);
}

static void esp_sqlite_uptime_ms_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    (void)argc;
    (void)argv;
    sqlite3_result_int64(context, (sqlite3_int64)(esp_timer_get_time() / 1000));
}

static void esp_sqlite_random_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    (void)argc;
    (void)argv;
    sqlite3_result_int64(context, (sqlite3_int64)(uint32_t)esp_random());
}

static void esp_sqlite_mem_used_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    (void)argc;
    (void)argv;
    sqlite3_result_int64(context, (sqlite3_int64)sqlite3_memory_used());
}

/* -------------------------------------------------------------------------
 * 注册
 * ------------------------------------------------------------------------- */

int esp_sqlite3_shox96_init(sqlite3 *db, char **pzErrMsg,
                           const sqlite3_api_routines *pThunk)
{
    (void)pThunk;

    int rc = SQLITE_OK;

    rc = sqlite3_create_function(db, "shox96_0_2c", 1,
                                 SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                 NULL, esp_sqlite3_shox96_compress, NULL, NULL);
    if (rc != SQLITE_OK) {
        if (pzErrMsg != NULL) {
            *pzErrMsg = sqlite3_mprintf("register shox96_0_2c failed: %s",
                                        sqlite3_errstr(rc));
        }
        return rc;
    }

    rc = sqlite3_create_function(db, "shox96_0_2d", 1,
                                 SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                 NULL, esp_sqlite3_shox96_decompress, NULL, NULL);
    if (rc != SQLITE_OK) {
        if (pzErrMsg != NULL) {
            *pzErrMsg = sqlite3_mprintf("register shox96_0_2d failed: %s",
                                        sqlite3_errstr(rc));
        }
        return rc;
    }

    /* 调试函数 */
    static const struct {
        const char *name;
        int n_arg;
        void (*fn)(sqlite3_context *, int, sqlite3_value **);
    } helpers[] = {
        { "esp_sqlite_version", 0, esp_sqlite_version_func },
        { "esp_sqlite_uptime_ms", 0, esp_sqlite_uptime_ms_func },
        { "esp_sqlite_random", 0, esp_sqlite_random_func },
        { "esp_sqlite_mem_used", 0, esp_sqlite_mem_used_func },
    };

    for (size_t i = 0; i < sizeof(helpers) / sizeof(helpers[0]); i++) {
        rc = sqlite3_create_function(db, helpers[i].name, helpers[i].n_arg,
                                     SQLITE_UTF8, NULL, helpers[i].fn, NULL, NULL);
        if (rc != SQLITE_OK) {
            if (pzErrMsg != NULL) {
                *pzErrMsg = sqlite3_mprintf("register %s failed: %s",
                                            helpers[i].name, sqlite3_errstr(rc));
            }
            return rc;
        }
    }

    return SQLITE_OK;
}

esp_err_t esp_sqlite3_ext_register(void)
{
    int rc = sqlite3_auto_extension((void (*)(void))esp_sqlite3_shox96_init);
    if (rc != SQLITE_OK) {
        ESP_SQLITE3_LOGE("sqlite3_auto_extension failed: rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_SQLITE3_LOGI("extensions registered: shox96_0_2c/d, esp_sqlite_version, esp_sqlite_uptime_ms, esp_sqlite_random, esp_sqlite_mem_used");
    return ESP_OK;
}

void esp_sqlite3_ext_unregister(void)
{
    (void)sqlite3_cancel_auto_extension((void (*)(void))esp_sqlite3_shox96_init);
}
