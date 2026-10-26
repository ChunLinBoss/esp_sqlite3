# esp_sqlite3 API 参考

> 头文件：`#include "esp_sqlite3.h"`
> 所有函数默认线程安全（`SQLITE_THREADSAFE=1` 时）；同一个 `sqlite3_stmt` 不要跨任务并发使用。

## 1. 生命周期

```c
esp_err_t esp_sqlite3_init(void);
void      esp_sqlite3_deinit(void);
bool      esp_sqlite3_is_initialized(void);
const char *esp_sqlite3_version(void);
void      esp_sqlite3_set_log_level(int level); // ESP_LOG_ERROR / WARN / INFO / DEBUG
```

- `esp_sqlite3_init()` 幂等；必须在任何 SQLite API 之前调用。
- `esp_sqlite3_deinit()` 前必须关闭所有连接；否则 `sqlite3_shutdown()` 返回 BUSY，
  组件会保持 initialized 状态并打印警告。
- `esp_sqlite3_version()` 返回 `"3.54.1"` 这类字符串。

## 2. 配置与打开

### 2.1 `esp_sqlite3_config_t`

```c
typedef struct {
    const char *path;             // 数据库路径；":memory:" 表示内存库
    const char *vfs;              // NULL = 默认 "esp_sqlite3"
    bool read_only;
    bool create;
    bool uri;                     // 允许 file: URI
    uint32_t busy_timeout_ms;     // 默认 5000
    uint32_t cache_size_kib;      // 0 = 编译默认（默认 64KiB）
    uint32_t page_size;           // 0 = 编译默认（默认 4096）
    esp_sqlite3_journal_mode_t journal_mode; // 默认 DEFAULT=保持现有模式
    esp_sqlite3_sync_mode_t synchronous;
    bool foreign_keys;            // 默认 true
    bool temp_store_memory;       // 默认 true
    bool exclusive_locking;
    bool defensive;               // SQLITE_DBCONFIG_DEFENSIVE
    bool query_only;
    bool auto_optimize;           // 打开后执行 PRAGMA optimize
} esp_sqlite3_config_t;
```

### 2.2 打开/关闭

```c
void      esp_sqlite3_config_init(esp_sqlite3_config_t *cfg);
esp_err_t esp_sqlite3_open(const esp_sqlite3_config_t *cfg, esp_sqlite3_t **out_db);
esp_err_t esp_sqlite3_open_simple(const char *path, esp_sqlite3_t **out_db);
void      esp_sqlite3_close(esp_sqlite3_t *db);
sqlite3  *esp_sqlite3_handle(esp_sqlite3_t *db);
const char *esp_sqlite3_path(const esp_sqlite3_t *db);
bool      esp_sqlite3_in_transaction(const esp_sqlite3_t *db);
```

`esp_sqlite3_open()` 会自动执行：

```sql
PRAGMA cache_size = -<cache_size_kib>;  -- 仅当 >0
PRAGMA page_size = <page_size>;         -- 仅当 >0
PRAGMA journal_mode = <mode>;           -- 非只读且 mode != DEFAULT
PRAGMA synchronous = <level>;
PRAGMA foreign_keys = ON/OFF;
PRAGMA temp_store = MEMORY/FILE;
PRAGMA locking_mode = EXCLUSIVE;        -- 可选
PRAGMA query_only = ON;                 -- 只读或 query_only
```

`PRAGMA page_size` 对已存在的数据库只影响后续 `VACUUM`，不会改变当前页大小。

### 2.3 枚举

```c
typedef enum {
    ESP_SQLITE3_JOURNAL_DEFAULT = -1, // 保持数据库现有模式（新建库为 DELETE）
    ESP_SQLITE3_JOURNAL_DELETE = 0,
    ESP_SQLITE3_JOURNAL_TRUNCATE,
    ESP_SQLITE3_JOURNAL_PERSIST,
    ESP_SQLITE3_JOURNAL_MEMORY,
    ESP_SQLITE3_JOURNAL_WAL,
    ESP_SQLITE3_JOURNAL_OFF,
} esp_sqlite3_journal_mode_t;

typedef enum {
    ESP_SQLITE3_SYNC_OFF = 0,
    ESP_SQLITE3_SYNC_NORMAL = 1,
    ESP_SQLITE3_SYNC_FULL = 2,
    ESP_SQLITE3_SYNC_EXTRA = 3,
} esp_sqlite3_sync_mode_t;
```

## 3. 执行 SQL

```c
esp_err_t esp_sqlite3_exec(esp_sqlite3_t *db, const char *sql);
esp_err_t esp_sqlite3_exec_cb(esp_sqlite3_t *db, const char *sql,
                             int (*callback)(void *, int, char **, char **),
                             void *arg);
```

- `esp_sqlite3_exec()` 适合 DDL、批量 DML、PRAGMA。
- 多条语句用分号分隔；失败时整个调用返回错误，已执行的语句不会自动回滚，
  需要事务时请使用 `esp_sqlite3_transaction()`。
- 错误会打印 `rc / extended rc / error offset / errmsg`。

### 3.1 查询辅助

```c
typedef esp_err_t (*esp_sqlite3_row_cb_t)(void *ctx, sqlite3_stmt *stmt, int row_index);

esp_err_t esp_sqlite3_query(esp_sqlite3_t *db, const char *sql,
                           esp_sqlite3_row_cb_t cb, void *ctx);
esp_err_t esp_sqlite3_query_int64(esp_sqlite3_t *db, const char *sql, int64_t *out);
esp_err_t esp_sqlite3_query_double(esp_sqlite3_t *db, const char *sql, double *out);
esp_err_t esp_sqlite3_query_text(esp_sqlite3_t *db, const char *sql,
                                char *buf, size_t buf_len);
```

- 无结果 / NULL：返回 `ESP_ERR_NOT_FOUND`。
- `esp_sqlite3_query_text()` 缓冲区不足返回 `ESP_ERR_INVALID_SIZE`。
- `esp_sqlite3_query()` 的回调返回非 `ESP_OK` 会立即中止查询并原样返回。

示例：

```c
static esp_err_t print_row(void *ctx, sqlite3_stmt *stmt, int row)
{
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    printf("row %d: id=%lld name=%s\n", row,
           (long long)sqlite3_column_int64(stmt, 0), name ? name : "");
    return ESP_OK;
}

ESP_ERROR_CHECK(esp_sqlite3_query(db, "SELECT id,name FROM device ORDER BY id;",
                                 print_row, NULL));
```

## 4. 预编译语句

```c
esp_err_t esp_sqlite3_prepare(esp_sqlite3_t *db, const char *sql, sqlite3_stmt **stmt);
esp_err_t esp_sqlite3_prepare_v2(esp_sqlite3_t *db, const char *sql, int sql_len,
                                sqlite3_stmt **stmt, const char **tail);
int       esp_sqlite3_step(sqlite3_stmt *stmt); // SQLITE_ROW / SQLITE_DONE / 错误
esp_err_t esp_sqlite3_reset(sqlite3_stmt *stmt);
esp_err_t esp_sqlite3_clear_bindings(sqlite3_stmt *stmt);
esp_err_t esp_sqlite3_finalize(sqlite3_stmt *stmt);

esp_err_t esp_sqlite3_bind_null(sqlite3_stmt *stmt, int index);
esp_err_t esp_sqlite3_bind_int(sqlite3_stmt *stmt, int index, int value);
esp_err_t esp_sqlite3_bind_int64(sqlite3_stmt *stmt, int index, int64_t value);
esp_err_t esp_sqlite3_bind_double(sqlite3_stmt *stmt, int index, double value);
esp_err_t esp_sqlite3_bind_text(sqlite3_stmt *stmt, int index, const char *text);
esp_err_t esp_sqlite3_bind_blob(sqlite3_stmt *stmt, int index, const void *data, int len);
esp_err_t esp_sqlite3_column_text_copy(sqlite3_stmt *stmt, int col,
                                      char *buf, size_t buf_len);
```

绑定文本/BLOB 使用 `SQLITE_TRANSIENT`，SQLite 会复制一份，调用者不需要保持指针。

示例（批量插入）：

```c
sqlite3_stmt *stmt = NULL;
ESP_ERROR_CHECK(esp_sqlite3_prepare(db,
    "INSERT INTO sample(ts, value) VALUES(?1, ?2);", &stmt));

ESP_ERROR_CHECK(esp_sqlite3_begin(db));
for (int i = 0; i < 100; i++) {
    esp_sqlite3_bind_int64(stmt, 1, 1700000000 + i);
    esp_sqlite3_bind_double(stmt, 2, i * 0.5);
    int rc = esp_sqlite3_step(stmt);
    if (rc != SQLITE_DONE) { /* handle error */ }
    esp_sqlite3_reset(stmt);
}
ESP_ERROR_CHECK(esp_sqlite3_commit(db));
esp_sqlite3_finalize(stmt);
```

## 5. 事务

```c
esp_err_t esp_sqlite3_begin(esp_sqlite3_t *db);           // BEGIN IMMEDIATE
esp_err_t esp_sqlite3_begin_deferred(esp_sqlite3_t *db);  // BEGIN DEFERRED
esp_err_t esp_sqlite3_commit(esp_sqlite3_t *db);
esp_err_t esp_sqlite3_rollback(esp_sqlite3_t *db);        // 不在事务中返回 ESP_OK

typedef esp_err_t (*esp_sqlite3_txn_cb_t)(void *ctx);
esp_err_t esp_sqlite3_transaction(esp_sqlite3_t *db,
                                 esp_sqlite3_txn_cb_t fn, void *ctx);
```

`esp_sqlite3_transaction()`：

- 先 `BEGIN IMMEDIATE`；
- `fn` 返回 `ESP_OK` → `COMMIT`；
- 否则 `ROLLBACK` 并返回 `fn` 的错误码；
- 适合批量写入，减少 Flash 擦写次数。

## 6. 维护与诊断

```c
esp_err_t esp_sqlite3_integrity_check(esp_sqlite3_t *db, bool quick,
                                     char *out, size_t out_len);
esp_err_t esp_sqlite3_vacuum(esp_sqlite3_t *db);
esp_err_t esp_sqlite3_vacuum_into(esp_sqlite3_t *db, const char *dest_path);
esp_err_t esp_sqlite3_wal_checkpoint(esp_sqlite3_t *db, int mode,
                                    int *log_frames, int *checkpointed_frames);
esp_err_t esp_sqlite3_optimize(esp_sqlite3_t *db);
esp_err_t esp_sqlite3_analyze(esp_sqlite3_t *db);
esp_err_t esp_sqlite3_backup(const char *src_path, const char *dst_path);
esp_err_t esp_sqlite3_get_size(esp_sqlite3_t *db, int64_t *bytes);
esp_err_t esp_sqlite3_get_user_version(esp_sqlite3_t *db, int *version);
esp_err_t esp_sqlite3_set_user_version(esp_sqlite3_t *db, int version);
esp_err_t esp_sqlite3_table_exists(esp_sqlite3_t *db, const char *table, bool *exists);
esp_err_t esp_sqlite3_set_busy_timeout(esp_sqlite3_t *db, uint32_t ms);
esp_err_t esp_sqlite3_get_mem_stats(esp_sqlite3_mem_stats_t *out);
```

- `integrity_check`：第一行结果不是 `"ok"` 时返回 `ESP_FAIL`，结果写入 `out`。
- `vacuum`：需要约等于数据库大小的空闲空间。
- `vacuum_into`：SQLite 3.27+，生成一致性副本。
- `wal_checkpoint`：`mode` 取 `SQLITE_CHECKPOINT_PASSIVE/FULL/RESTART/TRUNCATE`。
- `backup`：在线备份，分块 `sqlite3_backup_step(128)`，BUSY 重试约 1 秒。
- `get_size`：`page_count * page_size`（不含 WAL 中未 checkpoint 的页）。

### 6.1 内存统计

```c
typedef struct {
    int64_t memory_used;          // 当前 SQLite 堆内存（字节）
    int64_t memory_highwater;     // 历史峰值（字节）
    int64_t pagecache_used;       // 页缓存槽位数
    int64_t pagecache_highwater;  // 槽位峰值
    int64_t pagecache_overflow;   // 溢出次数
    int64_t malloc_count;         // 当前存活分配块数
} esp_sqlite3_mem_stats_t;
```

## 7. 数据库迁移

```c
typedef struct {
    int version;
    const char *sql;
} esp_sqlite3_migration_t;

esp_err_t esp_sqlite3_migrate(esp_sqlite3_t *db,
                             const esp_sqlite3_migration_t *steps, size_t count);
```

示例：

```c
static const esp_sqlite3_migration_t steps[] = {
    { 1, "CREATE TABLE device(id INTEGER PRIMARY KEY, name TEXT NOT NULL);" },
    { 2, "ALTER TABLE device ADD COLUMN online INTEGER NOT NULL DEFAULT 0;" },
    { 3, "CREATE INDEX idx_device_online ON device(online);" },
};

ESP_ERROR_CHECK(esp_sqlite3_migrate(db, steps, sizeof(steps) / sizeof(steps[0])));
```

每个版本独立事务；`PRAGMA user_version` 更新与 DDL 在同一事务内提交。
重复调用会跳过已执行的版本；`steps` 必须按 `version` 递增排列。

## 8. 错误处理

```c
const char *esp_sqlite3_errmsg(const esp_sqlite3_t *db);
int         esp_sqlite3_errcode(const esp_sqlite3_t *db);
int         esp_sqlite3_extended_errcode(const esp_sqlite3_t *db);
int         esp_sqlite3_error_offset(const esp_sqlite3_t *db); // 无则 -1
const char *esp_sqlite3_errstr(int rc);
esp_err_t   esp_sqlite3_rc_to_esp_err(int rc);
```

错误码映射：

| SQLite | esp_err_t |
|--------|-----------|
| `SQLITE_OK/ROW/DONE` | `ESP_OK` |
| `SQLITE_NOMEM/FULL` | `ESP_ERR_NO_MEM` |
| `SQLITE_BUSY/LOCKED` | `ESP_ERR_TIMEOUT` |
| `SQLITE_NOTFOUND/CANTOPEN` | `ESP_ERR_NOT_FOUND` |
| `SQLITE_MISUSE/RANGE` | `ESP_ERR_INVALID_ARG` |
| `SQLITE_READONLY/PERM/AUTH` | `ESP_ERR_NOT_ALLOWED` |
| `SQLITE_CORRUPT/NOTADB` | `ESP_ERR_INVALID_CRC` |
| `SQLITE_TOOBIG` | `ESP_ERR_INVALID_SIZE` |
| 其他 | `ESP_FAIL` |

需要精确 SQLite 错误码时，直接用 `esp_sqlite3_handle(db)` 调用原生 API。

## 9. VFS 接口

```c
#include "esp_sqlite3_vfs.h"

sqlite3_vfs *esp_sqlite3_vfs_get(void);
esp_err_t    esp_sqlite3_vfs_register(bool make_default);
void         esp_sqlite3_vfs_get_stats(esp_sqlite3_vfs_stats_t *out);
void         esp_sqlite3_vfs_reset_stats(void);
esp_err_t    esp_sqlite3_vfs_make_path(const char *path, char *out, size_t out_len);
```

统计字段：`open_files`、`total_opens`、`read_ops`、`write_ops`、`sync_ops`、
`truncate_ops`、`lock_busy`、`bytes_read`、`bytes_written`。

## 10. 扩展 SQL 函数

旧版兼容：

```sql
SELECT shox96_0_2c('重复文本...');       -- 返回 BLOB
SELECT shox96_0_2d(shox96_0_2c('...'));  -- 解压
```

新增：

```sql
SELECT esp_sqlite_version();      -- '3.54.1'
SELECT esp_sqlite_uptime_ms();    -- 开机毫秒数
SELECT esp_sqlite_random();       -- 32 位随机数
SELECT esp_sqlite_mem_used();     -- 当前 SQLite 堆内存
```

> 安全提示：`shox96_0_2d()` 对损坏/恶意 BLOB 没有输出长度校验，只能用于
> 自己写入的可信数据，不要直接解压外部输入。

## 11. 常用配方

### 11.1 键值配置表

```sql
CREATE TABLE IF NOT EXISTS kv(
  k TEXT PRIMARY KEY,
  v TEXT,
  updated_at INTEGER DEFAULT (strftime('%s','now'))
) WITHOUT ROWID;
```

```c
sqlite3_stmt *stmt;
esp_sqlite3_prepare(db, "INSERT INTO kv(k,v) VALUES(?1,?2) "
                       "ON CONFLICT(k) DO UPDATE SET v=excluded.v, updated_at=strftime('%s','now');",
                   &stmt);
esp_sqlite3_bind_text(stmt, 1, "wifi_ssid");
esp_sqlite3_bind_text(stmt, 2, ssid);
esp_sqlite3_step(stmt);
esp_sqlite3_finalize(stmt);
```

### 11.2 离线数据队列

```sql
CREATE TABLE IF NOT EXISTS offline_sample(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  ts INTEGER NOT NULL,
  temp REAL,
  humi REAL,
  uploaded INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_offline_uploaded ON offline_sample(uploaded, id);
```

上报成功后：

```sql
DELETE FROM offline_sample WHERE id <= ? AND uploaded = 1;
```

### 11.3 只读导出/查询

```c
esp_sqlite3_config_t cfg;
esp_sqlite3_config_init(&cfg);
cfg.path = "/db/app.db";
cfg.read_only = true;
cfg.query_only = true;
cfg.temp_store_memory = true;
esp_sqlite3_open(&cfg, &db);
```

### 11.4 定期维护

```c
// 每天一次或联网空闲时
esp_sqlite3_wal_checkpoint(db, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
esp_sqlite3_optimize(db);
// 空间碎片严重时（确保有足够空闲空间）
esp_sqlite3_vacuum(db);
```

## 12. 生命周期与线程规则

- `esp_sqlite3_t` 由 `esp_sqlite3_open()` 创建，`esp_sqlite3_close()` 释放；
- 关闭前必须 `sqlite3_finalize()` 所有语句；
- 不要在多任务间同时使用同一个 `sqlite3_stmt`；
- 多任务并发写：每个任务独立连接，依赖 VFS 文件锁 + `busy_timeout` 重试；
- `esp_sqlite3_init()` 只在启动时调用一次；`esp_sqlite3_deinit()` 只在关机/测试时调用。
