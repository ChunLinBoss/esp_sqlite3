# =============================================================================
# ESP SQLite3 组件
#
# 说明：
#   - sqlite3/sqlite3.c 为 SQLite 官方 amalgamation（3.54.1），保持原样，
#     只通过编译宏裁剪功能；
#   - src/ 下为 ESP-IDF 适配层（VFS、内存、互斥、扩展函数、高层封装）；
#   - 所有可调参数集中在本文件与 Kconfig 中，便于对比不同档位的体积。
# =============================================================================

set(ESP_SQLITE3_SRCS
    "sqlite3/sqlite3.c"
    "src/esp_sqlite3.c"
    "src/esp_sqlite3_mem.c"
    "src/esp_sqlite3_mutex.c"
    "src/esp_sqlite3_vfs.c"
    "src/esp_sqlite3_ext.c"
    "src/shox96_0_2.c"
)

if(CONFIG_ESP_SQLITE3_SELFTEST)
    list(APPEND ESP_SQLITE3_SRCS "src/esp_sqlite3_selftest.c")
endif()

idf_component_register(
    SRCS ${ESP_SQLITE3_SRCS}
    INCLUDE_DIRS "include" "sqlite3"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES esp_hw_support esp_timer
)

# -----------------------------------------------------------------------------
# SQLite 编译宏
#
# 注意：SQLITE_OMIT_* 必须在编译 sqlite3.c 时生效；同时也会影响我们自己的
#       适配层，所以统一用 PRIVATE 定义。
# -----------------------------------------------------------------------------
# PRAGMA synchronous 的默认值由 choice 决定，Kconfig 不直接给出数值
if(CONFIG_ESP_SQLITE3_SYNCHRONOUS_OFF)
    set(ESP_SQLITE3_SYNCHRONOUS 0)
elseif(CONFIG_ESP_SQLITE3_SYNCHRONOUS_NORMAL)
    set(ESP_SQLITE3_SYNCHRONOUS 1)
elseif(CONFIG_ESP_SQLITE3_SYNCHRONOUS_EXTRA)
    set(ESP_SQLITE3_SYNCHRONOUS 3)
else()
    set(ESP_SQLITE3_SYNCHRONOUS 2)
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SQLITE_CORE=1
    SQLITE_OS_OTHER=1
    SQLITE_THREADSAFE=$<IF:$<BOOL:${CONFIG_ESP_SQLITE3_THREADSAFE}>,1,0>
    SQLITE_SYSTEM_MALLOC=1

    # ---- 默认行为 -----------------------------------------------------------
    SQLITE_TEMP_STORE=$<IF:$<BOOL:${CONFIG_ESP_SQLITE3_TEMP_STORE_MEMORY}>,3,1>
    SQLITE_DEFAULT_PAGE_SIZE=${CONFIG_ESP_SQLITE3_PAGE_SIZE}
    SQLITE_MAX_DEFAULT_PAGE_SIZE=8192
    SQLITE_DEFAULT_CACHE_SIZE=-${CONFIG_ESP_SQLITE3_CACHE_KIB}
    SQLITE_DEFAULT_SYNCHRONOUS=${ESP_SQLITE3_SYNCHRONOUS}
    SQLITE_DEFAULT_FOREIGN_KEYS=$<IF:$<BOOL:${CONFIG_ESP_SQLITE3_FOREIGN_KEYS}>,1,0>
    SQLITE_SECURE_DELETE=$<IF:$<BOOL:${CONFIG_ESP_SQLITE3_SECURE_DELETE}>,1,0>
    SQLITE_DEFAULT_MEMSTATUS=$<IF:$<BOOL:${CONFIG_ESP_SQLITE3_MEMSTATUS}>,1,0>
    SQLITE_MAX_EXPR_DEPTH=${CONFIG_ESP_SQLITE3_MAX_EXPR_DEPTH}

    # ---- 嵌入式裁剪 ---------------------------------------------------------
    SQLITE_SMALL_STACK=1
    SQLITE_LIKE_DOESNT_MATCH_BLOBS=1
    SQLITE_DISABLE_LFS=1
    SQLITE_DISABLE_DIRSYNC=1
    SQLITE_DEFAULT_MMAP_SIZE=0
    SQLITE_MAX_MMAP_SIZE=0
    SQLITE_USE_URI=1
    SQLITE_OMIT_TCL_VARIABLE=1
)

# 各档位/开关对应的可选功能
if(CONFIG_ESP_SQLITE3_ENABLE_WAL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SQLITE_DEFAULT_WAL_SYNCHRONOUS=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SQLITE_OMIT_WAL=1)
endif()

if(CONFIG_ESP_SQLITE3_ENABLE_FTS5)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SQLITE_ENABLE_FTS5=1)
endif()
if(CONFIG_ESP_SQLITE3_ENABLE_RTREE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SQLITE_ENABLE_RTREE=1)
endif()
if(CONFIG_ESP_SQLITE3_ENABLE_STAT4)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SQLITE_ENABLE_STAT4=1)
endif()
if(CONFIG_ESP_SQLITE3_ENABLE_DBSTAT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SQLITE_ENABLE_DBSTAT_VTAB=1)
endif()
if(CONFIG_ESP_SQLITE3_ENABLE_MATH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SQLITE_ENABLE_MATH_FUNCTIONS=1)
endif()
if(CONFIG_ESP_SQLITE3_ENABLE_COLUMN_METADATA)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SQLITE_ENABLE_COLUMN_METADATA=1)
endif()
if(CONFIG_ESP_SQLITE3_ENABLE_BYTECODE_VTAB)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SQLITE_ENABLE_BYTECODE_VTAB=1)
endif()
if(CONFIG_ESP_SQLITE3_ENABLE_NORMALIZE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SQLITE_ENABLE_NORMALIZE=1)
endif()

if(CONFIG_ESP_SQLITE3_LOOKASIDE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SQLITE_DEFAULT_LOOKASIDE=256,32)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SQLITE_OMIT_LOOKASIDE=1)
endif()

if(NOT CONFIG_ESP_SQLITE3_UTF16)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SQLITE_OMIT_UTF16=1)
endif()
if(NOT CONFIG_ESP_SQLITE3_LOAD_EXTENSION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SQLITE_OMIT_LOAD_EXTENSION=1)
endif()
# 官方已废弃的 API 与共享缓存统一移除，不再提供 Kconfig 开关。
# 被移除的接口清单见 docs/00-功能清单.md 的“已移除的废弃 API”章节。
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SQLITE_OMIT_SHARED_CACHE=1
    SQLITE_OMIT_DEPRECATED=1
)
if(CONFIG_ESP_SQLITE3_NO_SYNC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SQLITE_NO_SYNC=1)
endif()

# -----------------------------------------------------------------------------
# CRUD 档位：只保留最基本的增删改查
#
# 以下宏会直接删除对应 SQL 能力，不能与 FTS5/RTREE/DBSTAT 等虚拟表同时使用；
# Kconfig 已把 CRUD 档位下的这些功能默认关闭并加 depends 限制。
# -----------------------------------------------------------------------------
if(CONFIG_ESP_SQLITE3_PROFILE_CRUD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        SQLITE_OMIT_JSON=1
        SQLITE_OMIT_DATETIME_FUNCS=1
        SQLITE_OMIT_AUTHORIZATION=1
        SQLITE_OMIT_AUTOVACUUM=1
        SQLITE_OMIT_EXPLAIN=1
        SQLITE_OMIT_FOREIGN_KEY=1
        SQLITE_OMIT_INTEGRITY_CHECK=1
        SQLITE_OMIT_PROGRESS_CALLBACK=1
        SQLITE_OMIT_TEMPDB=1
        SQLITE_OMIT_TRACE=1
        SQLITE_OMIT_GET_TABLE=1
        SQLITE_OMIT_INCRBLOB=1
    )
endif()

# 数学函数需要 libm
if(CONFIG_ESP_SQLITE3_ENABLE_MATH)
    target_link_libraries(${COMPONENT_LIB} INTERFACE m)
endif()

# -----------------------------------------------------------------------------
# 仅对官方 sqlite3.c 关闭一些“第三方代码风格”警告，避免污染构建日志。
# 我们自己的 src/ 仍然保持完整警告，方便发现问题。
#
# -include esp_sqlite3_force_ndebug.h：
#   ESP-IDF 5.x 默认开启 CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE，
#   会在 NDEBUG 下继续求值 assert(expr)，而 SQLite 的 assert 参数引用了
#   仅在 SQLITE_DEBUG 下存在的字段（如 mutexIsInit），导致 IDF5 编译失败。
# -----------------------------------------------------------------------------
set_source_files_properties("sqlite3/sqlite3.c" PROPERTIES
    COMPILE_OPTIONS
    "-include;${CMAKE_CURRENT_LIST_DIR}/src/esp_sqlite3_force_ndebug.h;-Wno-unused-parameter;-Wno-sign-compare;-Wno-unused-function;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-implicit-fallthrough;-Wno-missing-field-initializers;-Wno-format;-Wno-error"
)

# shox96 是第三方压缩算法（Apache-2.0），保留原风格，仅放宽少量风格告警
set_source_files_properties("src/shox96_0_2.c" PROPERTIES
    COMPILE_OPTIONS
    "-Wno-error=char-subscripts;-Wno-error=maybe-uninitialized;-Wno-error=implicit-fallthrough;-Wno-error=unused-but-set-variable;-Wno-error=sign-compare"
)
