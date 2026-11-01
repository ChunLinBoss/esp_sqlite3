# Changelog

## 0.3.2

### 新增

- 新增 `esp_sqlite3_set_progress_handler()`：业务 progress handler 通过内部
  trampoline 链接，`VACUUM` / `VACUUM INTO` / 在线备份等长操作复用业务步长，
  并在回调中喂任务看门狗、周期性让出 CPU；长操作结束后恢复业务 handler。
- 新增 `test_apps/stress` 压力工程：掉电重启、磁盘写满、只读 VFS 挂载、
  只读 WAL、mmap 关闭、VACUUM progress 保活。
- 自测新增只读 WAL、mmap=0、VACUUM progress handler 三项，共 26 项。

### 修复

- 只读连接打开活动 WAL 数据库时，若同进程已有写连接，
  `xShmMap()` 直接附着到已有 WAL shm，不再误返回 `SQLITE_READONLY_CANTINIT`
  导致 `disk I/O error`。

### 验证

- ESP-IDF 6.1 / ESP32-C5：`test_apps/stress` 全部 PASS：8 次掉电复位、
  磁盘写满、只读挂载读写拒绝、只读 WAL、mmap=0、VACUUM progress。
- ESP-IDF 6.1 / ESP32-C5：FULL/MINIMAL/STANDARD/MAX/CRUD 全部
  `SELFTEST RESULT: PASS`（每次擦除 flash）。
- ESP-IDF 5.5.4 / ESP32-C5：FULL `SELFTEST RESULT: PASS`；压力工程构建通过。

## 0.3.1

### 修复

- `esp_sqlite3_query()` / `query_int64()` / `query_double()` / `query_text()`
  对纯空白/注释 SQL 的 `stmt == NULL` 增加检查，避免对 NULL 语句调用
  `sqlite3_step()` 导致崩溃。
- VFS 锁状态机：`PENDING` 也要拒绝其他连接的 `EXCLUSIVE`；`EXCLUSIVE` 也要
  拒绝其他连接的 `PENDING`；`xCheckReservedLock()` 现在把
  RESERVED/PENDING/EXCLUSIVE 任一状态都计为写者。
- `esp_sqlite3_state_mutex` 改为静态创建，避免在 `portENTER_CRITICAL` 内做
  可能走堆分配的动态信号量创建。

### 验证

- ESP-IDF 6.1 / ESP32-C5 FULL：`SELFTEST RESULT: PASS`（含空 SQL 查询与
  双任务并发写回归）。

## 0.3.0

### 修复

- VFS 路径按目录项真实大小写归一化：FATFS 上 `App.db` / `app.db` 共用同一把
  文件锁和同一份 WAL shm；SPIFFS/LittleFS 上不同大小写仍是不同文件。
- `PERSIST_WAL` 从固定 4 项数组改为动态表，超过 4 个数据库不再误删 `-wal`。
- `xClose` 在 SQLite 未调用 `xShmUnmap` 时也释放本文件持有的 WAL shm 锁，避免
  残留锁导致其他连接 `SQLITE_BUSY`。
- 外部先调用 `sqlite3_initialize()` 时，`esp_sqlite3_init()` 会 shutdown 后重新
  安装分配器/FreeRTOS 互斥量；有外部连接时安全返回 BUSY。
- 运行时 `temp_store=FILE` 始终使用 `CONFIG_ESP_SQLITE3_TEMP_DIR`，不依赖编译
  默认是否为 `SQLITE_TEMP_STORE=3`。
- VACUUM / VACUUM INTO / 在线备份周期性让出 CPU 并喂任务看门狗；备份每步喂狗。
- shox96 直接 API 遇到不可编码字节返回错误，不再静默丢数据。
- `esp_sqlite3_query()` 增加 NOMEM 重试；`table_exists()` 检查绑定错误。
- 迁移步骤在执行前统一校验版本单调性和 SQL 非空，避免执行到一半才发现配置错误。
- 内存分配器 `xRoundup` 处理整数溢出；互斥量未初始化时 `xMutexAlloc` 返回 NULL。
- 损坏数据库在打开或 `integrity_check` 时明确返回错误。

### 自测新增

- 外部 `sqlite3_initialize()` 恢复。
- VFS 稀疏写、越过 EOF 短读、补零、`path=NULL` 临时文件、`xTruncate`。
- 大小写不同文件名的独立性。
- 运行时 `temp_store=FILE` 与临时目录。
- 损坏数据库拒绝。
- 双任务并发写同一数据库。
- `PERSIST_WAL` 连续 6 个数据库。
- FULL 创建 WAL 库，MINIMAL 打开时拒绝（WAL downgrade guard）。
- `deinit` 后重新 `init/open`。

### 验证

- ESP-IDF 6.1 / ESP32-C5：MAX、FULL、STANDARD、MINIMAL、CRUD 全部
  `SELFTEST RESULT: PASS`。
- ESP-IDF 5.5.4 / ESP32-C5：FULL、MINIMAL 硬件运行
  `SELFTEST RESULT: PASS`。
- 主机 shox96：5 万组随机 ASCII 往返、20 万组随机损坏输入、非 ASCII 拒绝、
  输出容量不足拒绝，全部通过。

## 0.2.2

- 组件清单支持全部 ESP-IDF 目标。
- 修正 README 安装版本与默认档位说明。

## 0.2.1

- 修正 registry README 安装版本。

## 0.2.0

- 修复 SPIFFS lseek/EOF 短读、稀疏写、临时文件名长度、只读写语义、WAL 只读
  映射、VFS 注册竞态、deinit 残留 VFS 状态。
- 修复 shox96 未初始化读、非 ASCII 静默丢数据、输出容量不足、70KB 以上编码
  边界、`SHX_ERR` 冲突。
- 修复 close BUSY、`clear_bindings(NULL)`、列文本 NULL/OOM、迁移顺序、备份
  同路径/错误传播、CRUD/WAL 能力返回值、URI/secure-delete 配置。

## 0.1.0

- 首次发布：SQLite 3.54.1 amalgamation、ESP-IDF VFS、WAL、PSRAM 分配器、
  FTS5/RTREE/STAT4/DBSTAT、shox96、五个功能档位。
