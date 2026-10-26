# 移植与 ESP-IDF 适配说明

> 目标：把 SQLite 官方 amalgamation 接到 ESP-IDF v5.x / v6.x，并保持可维护性。
> 已验证：ESP-IDF **5.5.4**（GCC 14.2）+ ESP32-C5；ESP-IDF **6.1**（GCC 15.2）+ ESP32-C5。

## 1. 获取官方源码

```powershell
# 以 3.54.1 为例
curl.exe -L -o sqlite-amalgamation-3540100.zip `
  https://sqlite.org/2026/sqlite-amalgamation-3540100.zip

# 校验 SHA3-256（官方下载页给出）
python -c "import hashlib;print(hashlib.sha3_256(open('sqlite-amalgamation-3540100.zip','rb').read()).hexdigest())"
# 期望：92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30

Expand-Archive sqlite-amalgamation-3540100.zip -DestinationPath sqlite
```

只把 `sqlite3.c`、`sqlite3.h`、`sqlite3ext.h` 放进组件的 `sqlite3/` 目录。
`shell.c` 不需要（固件里没有命令行 shell）。

## 2. 组件组成

```
esp_sqlite3/
├── CMakeLists.txt
├── Kconfig
├── sqlite3/{sqlite3.c,sqlite3.h,sqlite3ext.h}
├── include/{esp_sqlite3.h,esp_sqlite3_vfs.h}
├── src/
│   ├── esp_sqlite3.c
│   ├── esp_sqlite3_vfs.c
│   ├── esp_sqlite3_mem.c
│   ├── esp_sqlite3_mutex.c
│   ├── esp_sqlite3_ext.c
│   ├── esp_sqlite3_selftest.c
│   ├── esp_sqlite3_force_ndebug.h
│   ├── shox96_0_2.c
│   └── shox96_0_2.h
└── test_apps/selftest/
```

## 3. 关键编译宏

### 3.1 必须定义

| 宏 | 值 | 原因 |
|----|----|------|
| `SQLITE_CORE` | 1 | 作为核心库编译，而不是扩展 |
| `SQLITE_OS_OTHER` | 1 | 不编译 os_unix/os_win，由 `sqlite3_os_init()` 注册自定义 VFS |
| `SQLITE_THREADSAFE` | 1/0 | 1 = 串行化模式，使用我们的 FreeRTOS 互斥量 |
| `SQLITE_SYSTEM_MALLOC` | 1 | 允许 `SQLITE_CONFIG_MALLOC` 覆盖 |
| `SQLITE_TEMP_STORE` | 3/1 | 3 = 临时数据强制内存；1 = 允许文件 |
| `SQLITE_DEFAULT_PAGE_SIZE` | 4096 | Flash 友好 |
| `SQLITE_MAX_MMAP_SIZE` | 0 | ESP32 无通用文件 mmap |
| `SQLITE_USE_URI` | 1 | 支持 `file:` URI |
| `SQLITE_DISABLE_LFS` | 1 | 不需要大文件偏移特性 |
| `SQLITE_DISABLE_DIRSYNC` | 1 | ESP VFS 无目录 fsync |
| `SQLITE_SMALL_STACK` | 1 | 减小任务栈压力 |

### 3.2 可选功能

| 宏 | 作用 | 档位 |
|----|------|------|
| `SQLITE_DEFAULT_WAL_SYNCHRONOUS=1` | WAL 默认 NORMAL | FULL/STANDARD |
| `SQLITE_OMIT_WAL` | 关闭 WAL | MINIMAL / CRUD |
| `SQLITE_ENABLE_FTS5` | 全文检索 | FULL |
| `SQLITE_ENABLE_RTREE` | R*Tree | FULL/STANDARD |
| `SQLITE_ENABLE_STAT4` | 精确统计 | FULL |
| `SQLITE_ENABLE_DBSTAT_VTAB` | DBSTAT 虚拟表 | FULL/STANDARD |
| `SQLITE_ENABLE_MATH_FUNCTIONS` | 数学函数（需 `-lm`） | FULL/STANDARD |
| `SQLITE_ENABLE_COLUMN_METADATA` | 列元数据 API | 默认 |
| `SQLITE_ENABLE_BYTECODE_VTAB` | bytecode 虚拟表（调试） | 可选 |
| `SQLITE_ENABLE_NORMALIZE` | `sqlite3_normalized_sql()` | 可选 |
| `SQLITE_OMIT_UTF16` | 省 UTF-16 API | 默认 |
| `SQLITE_OMIT_LOAD_EXTENSION` | 不编译 dlopen 相关 | 默认 |
| `SQLITE_OMIT_SHARED_CACHE` | 移除已废弃的共享缓存 | **始终** |
| `SQLITE_OMIT_DEPRECATED` | 移除已废弃 API | **始终** |
| `SQLITE_NO_SYNC` | 跳过 fsync | **不要开** |

> `SQLITE_OMIT_SHARED_CACHE` 与 `SQLITE_OMIT_DEPRECATED` 已固定开启，不再提供
> Kconfig 开关。被移除的具体函数清单与替代方案见
> [00-功能清单](00-功能清单.md) 的“已移除的废弃 API”一节。

> `SQLITE_OMIT_*` 是编译期裁剪，不是运行时开关。切换档位后必须重新编译。

## 4. IDF 5 / IDF 6 差异与处理

### 4.1 `console` 组件已移除（IDF 6）

社区版 `CMakeLists.txt` 里有：

```cmake
PRIV_REQUIRES console spiffs spi_flash esp_timer
```

IDF 6 已没有 `console` 组件，直接导致配置失败。本组件只依赖：

```cmake
PRIV_REQUIRES esp_hw_support esp_timer
```

- `esp_hw_support` 提供 `esp_random()`；
- `esp_timer` 提供 `esp_timer_get_time()`；
- 不直接依赖 `spiffs`/`fatfs`/`sdmmc`，因此存储方案可自由替换。

### 4.2 `spi_flash_mmap.h` / `SPI_FLASH_SEC_SIZE`

社区版用 `SPI_FLASH_SEC_SIZE` 作为 `xSectorSize`，IDF6 中该头文件/宏的位置和
可见性发生变化。本组件改用 Kconfig：

```c
CONFIG_ESP_SQLITE3_SECTOR_SIZE  // 默认 4096
```

不包含任何 `spi_flash_*` 头文件。

### 4.3 `assert()` 在 NDEBUG 下仍求值（IDF 5）

ESP-IDF 5.x 的 `assert.h` 有：

```c
#if defined(NDEBUG)
  #if CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE
    #define assert(__e) ((void)(__e))   // 仍然求值！
  #else
    #define assert(__e) ((void)0)
  #endif
```

SQLite 的 amalgamation 假设 `NDEBUG` 下 assert 参数不被求值，于是出现：

```
sqlite3.c:30673: error: 'mutexIsInit' undeclared
sqlite3.c:102932: error: 'VdbeCursor' has no member named 'seekOp'
```

这些字段只在 `SQLITE_DEBUG` 下存在。

解决办法：只对 `sqlite3.c` 强制包含
`src/esp_sqlite3_force_ndebug.h`：

```c
#pragma once
#include "sdkconfig.h"
#undef CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE
```

CMake：

```cmake
set_source_files_properties("sqlite3/sqlite3.c" PROPERTIES
    COMPILE_OPTIONS
    "-include;${CMAKE_CURRENT_LIST_DIR}/src/esp_sqlite3_force_ndebug.h;..."
)
```

> 不要全局关闭 `CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE`，那会影响整个工程；
> 只对 SQLite 这个第三方 amalgamation 做局部处理。

### 4.4 `esp_random.h` 归属

IDF 5/6 中 `esp_random()` 声明在 `esp_hw_support/include/esp_random.h`，
因此组件 `PRIV_REQUIRES esp_hw_support`。

### 4.5 VFS 上下文 API 变化

IDF 5/6 逐步把 `esp_vfs_*` 的“无上下文”回调标记为 deprecated，并新增
`*_p` 版本。本组件**不直接注册 VFS 驱动**，只使用 POSIX 文件接口，
因此不受这些变化影响。

### 4.6 `-Werror` 与第三方代码

IDF 默认 `-Werror=all`。官方 `sqlite3.c` 和 shox96 会触发一些风格告警：

- `sqlite3.c`：`-Wno-unused-parameter`、`-Wno-sign-compare`、
  `-Wno-unused-function`、`-Wno-unused-variable`、`-Wno-format`、`-Wno-error`；
- `shox96_0_2.c`：`-Wno-error=char-subscripts`、`-Wno-error=maybe-uninitialized` 等。

我们自己的 `src/*.c` 保持完整警告，便于发现问题。

## 5. VFS 实现检查清单

实现 SQLite VFS 时逐项确认：

- [x] `sqlite3_vfs.iVersion` = 3，提供 `xCurrentTimeInt64`；
- [x] `szOsFile` = 自定义文件对象大小；
- [x] `mxPathname` > 0，路径拼接不越界；
- [x] `xOpen` 清零整个 `szOsFile`，设置静态 `pMethods`；
- [x] `xRead` 处理短读：补 0 + `SQLITE_IOERR_SHORT_READ`；
- [x] `xWrite` 循环处理短写；
- [x] `xTruncate` 调 `ftruncate`；
- [x] `xSync` 调 `fsync`（可配置关闭）；
- [x] `xFileSize` 用 `fstat`；
- [x] `xLock/xUnlock` 实现 SHARED/RESERVED/PENDING/EXCLUSIVE；
- [x] `xCheckReservedLock` 不把自己的 RESERVED 算作冲突；
- [x] `xFileControl` 处理 `TEMPFILENAME/VFSNAME/HAS_MOVED/PERSIST_WAL`；
- [x] `xSectorSize` 返回 2 的幂；
- [x] `xDeviceCharacteristics` 保守返回 0；
- [x] WAL：`xShmMap/xShmLock/xShmBarrier/xShmUnmap`；
- [x] `xDl*` 返回 NULL/no-op；
- [x] `xRandomness` 用硬件随机数；
- [x] `xSleep` 在调度器运行时用 `vTaskDelay`，否则用 ROM 延时；
- [x] `xCurrentTime/Int64` 用 `gettimeofday`；
- [x] `sqlite3_os_init/end` 注册/重置 VFS。

## 6. 自测工程编译命令

### 6.1 ESP-IDF 6.1

```powershell
$env:IDF_PATH='O:\Env\espidf610\v6.1\esp-idf'
$env:IDF_TOOLS_PATH='C:\Espressif\tools'
$env:IDF_PYTHON_ENV_PATH='C:\Espressif\tools\python\v6.1\venv'
$env:ESP_IDF_VERSION='6.1.0'
$env:ESP_ROM_ELF_DIR='C:\Espressif\tools\esp-rom-elfs\20241011'
$env:PATH="C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;" +
          "C:\Espressif\tools\cmake\4.0.3\bin;" +
          "C:\Espressif\tools\ninja\1.12.1;" +
          "C:\Espressif\tools\idf-exe\1.0.3;" +
          "C:\Espressif\tools\python\v6.1\venv\Scripts;$env:PATH"

cd O:\Soft\ESPIDF_Project\FmC5Demo\components\esp_sqlite3\test_apps\selftest
& "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" set-target esp32c5
& "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" build
```

### 6.2 ESP-IDF 5.5.4

```powershell
$env:IDF_PATH='O:\Env\espidf554\v5.5.4\esp-idf'
$env:IDF_PYTHON_ENV_PATH='C:\Espressif\tools\python\v5.5.4\venv'
$env:ESP_IDF_VERSION='5.5.4'
$env:PATH="C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;" + ...
```

### 6.3 烧录与运行

```powershell
idf.py -p COM3 flash monitor
```

期望输出：

```
I (...) esp_sqlite3: SQLite 3.54.1 initialized (vfs=esp_sqlite3, temp_store=memory, threadsafe=1)
I (...) esp_sqlite3: VFS registered: esp_sqlite3 (mxPathname=128)
I (...) esp_sqlite3: opened /db/selftest.db (...)
I (...) esp_sqlite3_test: journal_mode = wal
...
I (...) esp_sqlite3_test: ===== esp_sqlite3 selftest PASSED =====
I (...) selftest_main: SELFTEST RESULT: PASS
```

## 7. 常见问题排查

| 现象 | 可能原因 | 处理 |
|------|----------|------|
| `no such vfs: esp_sqlite3` | 未调用 `esp_sqlite3_init()`，或 `sqlite3_shutdown()` 后未重新注册 | 先 init；重新 init 会重新注册 |
| `SQLITE_CANTOPEN` | 挂载点不存在/路径拼错/文件系统未挂载 | 先挂载，检查 `esp_sqlite3_vfs_make_path()` 结果 |
| `database is locked` | 两个连接同时写，busy_timeout 太短 | 增大 `busy_timeout_ms`；避免长事务 |
| `disk I/O error` | `fsync/ftruncate` 不支持，或 Flash 满 | 检查 FS；用 `esp_spiffs_info` 看剩余空间 |
| `database disk image is malformed` | 旧版 RAM journal + 掉电损坏 | 用 `PRAGMA integrity_check`；从备份恢复 |
| `out of memory` | 页缓存/排序/PSRAM 不足 | 降 `cache_size_kib`；关 FTS5；检查 PSRAM |
| `stack overflow` | 复杂 SQL/递归 CTE/触发器 | 增大任务栈；限制递归深度 |
| WAL 打开失败 | MINIMAL 档位关闭了 WAL | 切 FULL/STANDARD/MAX 或 `journal_mode=DELETE` |
| `PRAGMA foreign_keys` 无效 | 旧版编译宏 `SQLITE_OMIT_FOREIGN_KEY` | 本组件默认不 OMIT；确认档位 |
| IDF5 编译报 `mutexIsInit` 未声明 | `CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE` | 确认 `esp_sqlite3_force_ndebug.h` 被 `-include` |
| IDF6 编译报找不到 `console` | 用了社区版 CMakeLists | 用本组件的 CMakeLists |

## 8. 添加自定义扩展

### 8.1 编译进固件的扩展

1. 新增 `src/esp_myext.c`；
2. 实现 `int esp_myext_init(sqlite3 *db, char **err, const sqlite3_api_routines *thunk)`；
3. 在 `esp_sqlite3_ext_register()` 中追加：
   ```c
   sqlite3_auto_extension((void (*)(void))esp_myext_init);
   ```
4. 在 `ESP_SQLITE3_SRCS` 中加入源文件；
5. 在 `esp_sqlite3_ext_unregister()` 中调用 `sqlite3_cancel_auto_extension()`。

### 8.2 使用官方 ext/misc

SQLite 源码树的 `ext/misc/` 下有很多单文件扩展（如 `carray.c`、`series.c`、
`regexp.c`）。它们通常支持 `SQLITE_EXTENSION_INIT1`/`sqlite3_api_routines`，
可以直接编译进固件并注册。注意：

- 不要使用 `sqlite3_load_extension()`（无 dlopen）；
- 扩展的 `sqlite3_api_routines` 参数可以传 NULL（静态链接模式）；
- 检查扩展是否依赖 `SQLITE_ENABLE_*` 宏。

## 9. 升级/维护建议

- 每季度跟进 SQLite 官方发布；
- 升级前备份 `sdkconfig.defaults` 与分区表；
- 升级后必跑自测工程；
- 对比 `libesp_sqlite3.a` 体积，超过预算时切换档位；
- 保留 `sqlite3/` 原样，所有适配改动放在 `src/`，便于 diff。
