# esp_sqlite3 压力/掉电/磁盘满/只读测试

该工程使用 4MB Flash 的 2MB app + 1MB SPIFFS 分区，专门验证：

1. 写事务过程中连续 8 次 `esp_restart()`，每次启动做 `integrity_check` 与
   批次行数校验；
2. 填满 SPIFFS 后 SQLite 返回 `SQLITE_FULL`/`SQLITE_IOERR`，清理后可恢复写入；
3. `/ro -> /db` 只读 VFS 挂载的 `EROFS` 降级与写入拒绝；
4. `PRAGMA mmap_size == 0`；
5. 业务 progress handler 在 VACUUM 后仍然有效。

```powershell
cd components/esp_sqlite3/test_apps/stress
idf.py set-target esp32c5
idf.py erase-flash      # 清除旧 NVS 阶段和 SPIFFS
idf.py flash monitor
```

期望串口最终输出：

```
I (...) stress: STRESS RESULT: PASS
```

注意：

- `erase-flash` 会删除 NVS 中的测试阶段标记；不擦除时，已完成的设备会直接
  打印 PASS。
- 这里的“掉电”是 MCU 复位（`esp_restart()`），不是物理断电。真实拔电需外部
  继电器/人工断电；复位测试覆盖了 WAL/SPIFFS 在写入中断后的恢复路径。
- 只读挂载是测试工程内的 VFS 包装器，不依赖 FATFS 只读镜像；它验证的是
  ESP-IDF VFS 的 `ESP_VFS_FLAG_READONLY_FS` 与 SQLite VFS 的只读降级路径。
