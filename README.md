# esp_sqlite3 —— ESP-IDF 官方 SQLite 3.54.1 移植

面向 ESP32 / ESP-IDF v5.x ~ v6.x 的 SQLite3 组件。直接使用 SQLite 官方
amalgamation（`sqlite3.c` 3.54.1，SHA3 已校验），在其上实现完整的 ESP-IDF VFS
适配层、内存/互斥实现、扩展函数和中文高层 API。

- **当前功能清单**：[docs/00-功能清单.md](docs/00-功能清单.md)
- **Flash 建议**：并入主固件建议 **8MB 及以上**，16MB 更宽裕；4MB 仅适合
  独立自测固件。
- **均衡擦写说明**：SQLite 本身不做 WL，由底层文件系统提供（SPIFFS 内建、
  FATFS 需 `wear_levelling`、LittleFS 内建），详见功能清单第 8 节。

## 安装

从 ESP Component Registry 添加：

```bash
idf.py add-dependency "chunlinboss/esp_sqlite3^0.3.3"
```

或在项目 `main/idf_component.yml` 中声明：

```yaml
dependencies:
  chunlinboss/esp_sqlite3: "^0.3.3"
```

## 为什么重做

社区版 `siara-cc/esp32-idf-sqlite3` 与 `nopnop2002/esp32-idf-sqlite3` 基于
SQLite **3.25.2（2018 年）**，并且为了省空间 `#define` 了大量 `SQLITE_OMIT_*`，
存在以下问题：

| 项目 | 社区版 | esp_sqlite3 |
|------|--------|------------|
| SQLite 版本 | 3.25.2（2018） | **3.54.1（官方最新）** |
| 文件锁 | `xLock`/`xUnlock` 空实现 | 完整 SHARED/RESERVED/PENDING/EXCLUSIVE 状态机 |
| 回滚日志 | 放 RAM 链表，掉电即丢 | 真实 `.journal` 文件，掉电可恢复 |
| 截断 | `xTruncate` 空实现（VACUUM 失效） | `ftruncate()` |
| WAL | `SQLITE_OMIT_WAL` | 完整 `xShmMap/xShmLock` 实现 |
| 外键 | `SQLITE_OMIT_FOREIGN_KEY` | 默认开启 |
| 完整性检查 | `SQLITE_OMIT_INTEGRITY_CHECK` | 保留 |
| JSON | 未启用 | 3.38+ 内建 |
| FTS5 / RTREE / STAT4 / DBSTAT | 无 | 按档位可选 |
| 线程安全 | `SQLITE_THREADSAFE=0` | FreeRTOS 递归互斥，默认开启 |
| PSRAM | 宏替换 malloc | `SQLITE_CONFIG_MALLOC` + PSRAM 优先/自动回退 |
| IDF 5/6 | 只到 IDF 5.5，IDF6 编译失败 | **IDF 5.5.4 与 6.1 均已编译验证** |
| 压缩扩展 | shox96 | **保留 shox96_0_2c/d**，另加 `esp_sqlite_*` 辅助函数 |

详细的调研与逐项对比见
[`docs/01-调研与功能对比.md`](docs/01-调研与功能对比.md)。

## 目录结构

```
components/esp_sqlite3/
├── CMakeLists.txt              # 编译宏、档位映射、第三方警告豁免
├── Kconfig                     # MAX / FULL / STANDARD / MINIMAL / CRUD 与所有开关
├── sqlite3/                    # 官方 amalgamation，保持原样
│   ├── sqlite3.c
│   ├── sqlite3.h
│   └── sqlite3ext.h
├── include/
│   ├── esp_sqlite3.h            # 高层封装 API（中文注释）
│   └── esp_sqlite3_vfs.h        # VFS 注册、统计、路径工具
├── src/
│   ├── esp_sqlite3.c            # 生命周期/打开/执行/事务/维护/迁移
│   ├── esp_sqlite3_vfs.c        # 文件锁、日志、WAL 共享内存、POSIX 适配
│   ├── esp_sqlite3_mem.c        # PSRAM 优先的内存分配器
│   ├── esp_sqlite3_mutex.c      # FreeRTOS 递归互斥量
│   ├── esp_sqlite3_ext.c        # shox96 与 esp_sqlite_* SQL 函数
│   ├── esp_sqlite3_selftest.c   # 内置自测（可选编译）
│   ├── esp_sqlite3_force_ndebug.h # IDF5 assert 求值兼容
│   └── shox96_0_2.c/.h         # 旧版兼容压缩算法（Apache-2.0）
├── partitions/                 # 8MB / 16MB Flash 推荐分区表
└── test_apps/selftest/         # 独立自测工程（4MB Flash + SPIFFS）
```

## 快速使用

1. 把组件目录放到工程的 `components/esp_sqlite3` 下，或直接用
   `idf.py add-dependency` 添加。
2. 在 `main/CMakeLists.txt` 的 `REQUIRES` 中加入 `esp_sqlite3`。
3. 在挂载好文件系统后初始化并打开数据库：

```c
#include "esp_sqlite3.h"

ESP_ERROR_CHECK(esp_sqlite3_init());

esp_sqlite3_t *db = NULL;
esp_sqlite3_config_t cfg;
esp_sqlite3_config_init(&cfg);
cfg.path = "/db/app.db";          // 需要先挂载 SPIFFS/FATFS/LittleFS
cfg.journal_mode = ESP_SQLITE3_JOURNAL_WAL;
cfg.synchronous = ESP_SQLITE3_SYNC_NORMAL;
cfg.cache_size_kib = 16;
ESP_ERROR_CHECK(esp_sqlite3_open(&cfg, &db));

ESP_ERROR_CHECK(esp_sqlite3_exec(db,
    "CREATE TABLE IF NOT EXISTS kv(k TEXT PRIMARY KEY, v TEXT);"));

esp_sqlite3_close(db);
```

`esp_sqlite3_t` 内部持有 `sqlite3*`，需要高级功能时可调用
`esp_sqlite3_handle()` 拿到底层句柄。

## 功能档位与体积

在 `idf.py menuconfig -> ESP SQLite3` 中切换，或直接在 `sdkconfig.defaults` 写：

| 档位 | 功能概述 | 静态库体积* | 最小示例固件* |
|------|----------|-------------|----------------|
| MAX | 全部**未废弃**的可选功能（含 UTF-16、bytecode 虚表等） | **586.8 KB** | **836.3 KB** |
| FULL | WAL + FTS5 + RTREE + STAT4 + DBSTAT + 数学函数 | **569.9 KB** | **818.7 KB** |
| STANDARD | WAL + RTREE + DBSTAT + 数学函数 | **470.6 KB** | **713.8 KB** |
| MINIMAL | 核心 SQL + JSON，无 WAL/FTS5/RTREE/STAT4/DBSTAT/数学函数 | **432.4 KB** | **648.4 KB** |
| CRUD | 仅基础增删改查，关闭 JSON/日期函数/外键/完整性检查/WAL/FTS5 等 | **367.9 KB** | **581.9 KB** |

\* 同一最小示例固件（SPIFFS + 一次 CRUD smoke，**不含自测代码**），
ESP-IDF 6.1、`-Os`、ESP32-C5；shox96 始终包含。1 KB = 1024 B，
保留一位小数；静态 RAM 约 5.8~9.3KB，运行时页缓存默认 16KB/连接。

调试构建（IDF Debug / `-Og`）的最大值：

| 配置 | 静态库 | 固件 |
|------|--------|------|
| MAX 全功能 | 661.2 KB | 934.7 KB |
| CRUD | 412.1 KB | 650.1 KB |

> **已移除的废弃 API**：所有档位固定定义 `SQLITE_OMIT_DEPRECATED` 与
> `SQLITE_OMIT_SHARED_CACHE`，不再提供 `sqlite3_aggregate_count`、
> `sqlite3_expired`、`sqlite3_transfer_bindings`、`sqlite3_global_recover`、
> `sqlite3_thread_cleanup`、`sqlite3_memory_alarm`、`sqlite3_trace`、
> `sqlite3_profile`、`sqlite3_enable_shared_cache`。替代方案与“声明仍在但
> 无实现”的注意事项见
> [`docs/00-功能清单.md`](docs/00-功能清单.md) 的“已移除的废弃 API”一节。

> CRUD 档位关闭了当前 SQLite 3.54.1 amalgamation 下**可安全关闭**的非 CRUD
> 能力；视图、触发器、ALTER、ATTACH、VACUUM 等因预生成 parser 限制仍保留，
> 详见 [`docs/00-功能清单.md`](docs/00-功能清单.md)。

> **Flash 建议**：组件本身可以在 4MB 自测工程中运行；**并入主固件建议使用
> 8MB 及以上 Flash**，16MB 更宽裕。当前 4MB 布局的 `ota_0/ota_1` 各 1792KB，
> 主固件已约 1536KB，即使 CRUD 档位（库约 368KB）链接后也约 1904KB，放不进
> 现有 app 分区。
>
> - 8MB：[`partitions/partitions_8mb_sqlite.csv`](partitions/partitions_8mb_sqlite.csv)
>   （双 2.5MB OTA + 约 2.9MB SPIFFS）
> - 16MB：[`partitions/partitions_16mb_sqlite.csv`](partitions/partitions_16mb_sqlite.csv)
>   （双 4MB OTA + 4MB SPIFFS）
>
> 分区表变更**不能只靠 OTA 下发**，需要 USB 全片烧录一次。详见
> [`docs/02-架构设计.md`](docs/02-架构设计.md)。

## 兼容性

- ESP-IDF **v5.5.4**：FULL / MINIMAL 已在 ESP32-C5 上烧录运行，自测 PASS
  （GCC 14.2）。
- ESP-IDF **v6.1**：编译通过（GCC 15.2，ESP32-C5）；MAX / FULL / STANDARD /
  MINIMAL / CRUD 五档已在 C5 开发板逐档烧录，自测全部 PASS。
- 目标芯片：ESP32-C5（RISC-V）；代码不使用 Xtensa 专用指令，ESP32/S3/C3/C6
  等同样适用。
- 文件系统：任何实现了 POSIX `open/read/write/lseek/close/ftruncate/fsync/
  stat/unlink` 的 ESP-IDF VFS 挂载点，如 SPIFFS、FATFS、LittleFS、SD 卡。
  同一路径的不同大小写写法会按目录项中的真实名称归一化（FATFS 必需），
  SPIFFS/LittleFS 上不存在的错误大小写仍会正常返回 `CANTOPEN`。

### 边界与限制

- **只读 WAL**：同一进程存在活动写连接并建立 WAL shm 索引时，只读连接可以
  附着读取；如果进程重启后只剩一个未 checkpoint 的 WAL 且没有任何 shm，
  只读连接无法初始化索引，需要先由可写连接恢复/ checkpoint。
- **mmap**：组件固定 `SQLITE_MAX_MMAP_SIZE=0`，`PRAGMA mmap_size` 始终为 0。
- **多进程文件锁**：ESP-IDF 是单进程环境，VFS 锁是进程内状态机；多进程
  语义不适用。多任务并发写已通过自测。

### 0.3.x 可靠性行为

- **外部初始化**：即使应用先调用了 `sqlite3_initialize()`，`esp_sqlite3_init()`
  也会检测并用自定义分配器/FreeRTOS 互斥量重新初始化；存在外部连接时
  shutdown 返回 BUSY，不会强行破坏。
- **WAL 降级保护**：编译时没有 WAL 的固件打开 WAL 主库会返回
  `ESP_ERR_NOT_SUPPORTED`，不会读到未回放的旧数据。
- **PERSIST_WAL**：持久 WAL 标记使用动态表，不再受固定 4 个数据库限制。
- **临时库目录**：即使编译默认 `temp_store=MEMORY`，运行时设置
  `temp_store_memory=false` 也会使用 `CONFIG_ESP_SQLITE3_TEMP_DIR`。
- **长操作**：VACUUM / VACUUM INTO / 在线备份会周期性让出 CPU 并喂任务
  看门狗；备份仍分块执行，BUSY 重试上限约 1 秒。
- **VFS 边界**：SPIFFS 稀疏写、越过 EOF 的短读、路径 `NULL` 临时文件、
  关闭时清理 WAL shm 锁、deinit/reinit 均已加入自测；相对路径不允许用
  `..` 跳出 `CONFIG_ESP_SQLITE3_DEFAULT_DIR`。

## 自测

```powershell
cd components/esp_sqlite3/test_apps/selftest
idf.py set-target esp32c5
idf.py build flash monitor
```

## 压力 / 掉电 / 磁盘满 / 只读测试

```powershell
cd components/esp_sqlite3/test_apps/stress
idf.py set-target esp32c5
idf.py erase-flash
idf.py flash monitor
```

`stress` 工程会连续 8 次在写事务进行中复位，每次启动校验 `integrity_check`
和批次完整性；随后填满 SPIFFS 验证 `SQLITE_FULL` 恢复、用只读 VFS 挂载验证
`EROFS` 降级、验证 mmap 关闭，以及业务 progress handler 在 VACUUM 后仍有效。
期望最后一行是 `STRESS RESULT: PASS`。真实拔电需外部继电器；`esp_restart()`
覆盖了写入过程中的硬件复位恢复路径。

自测覆盖：建表、事务批量写入、UPDATE/DELETE、JSON、FTS5、RTREE、
shox96 往返、完整性检查、VACUUM、在线备份、user_version 迁移、WAL checkpoint、
内存统计；0.3.x 额外覆盖外部 `sqlite3_initialize` 恢复、WAL 降级拒绝、
SPIFFS 稀疏写/短路读、`path=NULL` 临时文件、大小写路径、运行时
`temp_store=FILE`、损坏库、双任务并发写、PERSIST_WAL x6、deinit/reinit；
以及只读 WAL、mmap=0、业务 progress handler 在 VACUUM 后保活。
期望串口最后输出：

```
I (...) esp_sqlite3_test: ===== esp_sqlite3 selftest PASSED =====
I (...) selftest_main: SELFTEST RESULT: PASS
```

## 文档

- [当前功能清单](docs/00-功能清单.md)
- [调研与功能对比](docs/01-调研与功能对比.md)
- [架构设计](docs/02-架构设计.md)
- [API 参考](docs/03-API参考.md)
- [移植与 IDF 适配](docs/04-移植与IDF适配.md)
- [测试与性能](docs/05-测试与性能.md)
- [变更记录](CHANGELOG.md)

## 许可证

- `sqlite3/`：SQLite 官方 amalgamation，**Public Domain**。
- `src/shox96_0_2.*`：Apache License 2.0，作者 Arundale Ramanathan。
- 其余新增代码：MIT License。详见 [`LICENSES.md`](LICENSES.md)。
