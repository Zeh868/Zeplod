# AGENTS.md - Zeplod（Zephyr 事件驱动应用）

本文件为在此仓库中运行的 AI 编码代理提供操作指南。

---

## 1. 构建命令

### 前置条件
- Zephyr SDK (0.16.x+)
- CMake (3.20.0+)
- West (Zephyr 构建工具)
- Python 3.8+

设置 `ZEPHYR_BASE` 环境变量或在仓库根目录配置 `zephyr_config.env`。
配置文件模板：`zephyr_config.env.template` → 复制为 `zephyr_config.env` 后编辑路径。

### 主应用程序构建
```bash
# 为目标板构建
west build -b <board> .

# 构建主机仿真（Zephyr 4.x 用 native_sim；CI 3.6 用 native_posix）
west build -b native_sim .
# west build -b native_posix .

# 使用自定义配置叠加文件构建
west build -b <board> . -- -DEXTRA_CONF_FILE=conf/examples/module_ipc.conf

# 清理并重新构建
west build -t pristine
west build -b <board> .
```

### 单元测试（native_sim / native_posix）
```bash
# 先激活环境（或直接用 run_tests，其内会自动 source setup_env）
source scripts/setup_env.sh     # Linux/macOS/WSL
# .\scripts\setup_env.ps1       # Windows PowerShell

# 推荐：自动激活环境 + 选择板型（native_sim 优先）
./scripts/run_tests.sh          # 默认 CONF：prj.conf;prj_test_extensions.conf
# .\scripts\run_tests.ps1       # Windows

# 硬件板测试（精简 + 640KB SRAM overlay，如 nucleo_l4r5zi）
west build -b nucleo_l4r5zi tests/ --build-dir build_tests_hw -p always -- "-DCONF_FILE=prj.conf"
# 含并发压测：叠加 prj_concurrency_stress.conf（勿在 192KB RAM 下使用）

# 手动（Zephyr 4.x）
west build -b native_sim tests/ --build-dir build_tests
west build -t run --build-dir build_tests

# CI 对齐（Zephyr 3.6 容器）
west build -b native_posix tests/ --build-dir build_tests
west build -t run --build-dir build_tests

# 全量 IPC + Slab（native_sim）
west build -b native_sim tests/ --build-dir build_tests -- -DCONF_FILE="prj.conf;prj_native_sim.conf"
west build -t run --build-dir build_tests

# 带覆盖率运行（将板型换为当前环境可用项）
west build -b native_sim tests/ --build-dir build_tests -- -DCMAKE_C_FLAGS="--coverage"
west build -t run --build-dir build_tests
gcovr -r .. --html --html-details coverage.html
```

### 烧录与监控
```bash
west flash          # 烧录到目标板
west console        # 监控串口输出
```

---

## 2. 代码质量工具

### 格式化代码（clang-format）
格式化配置：`.clang-format`（仓库根目录，基于 LLVM 风格）

```bash
# 格式化单个文件
clang-format -i src/path/to/file.c

# 格式化所有源文件（通过 pre-commit）
pip install pre-commit && pre-commit run --all-files
```

### 静态分析（clang-tidy）
分析配置：`.clang-tidy`（仓库根目录）

```bash
# 需要从构建生成的 compile_commands.json
clang-tidy -p build src/core/event_system.c
```

### Pre-commit 钩子
配置于 `.pre-commit-config.yaml`：
- trailing-whitespace（尾随空白）
- end-of-file-fixer（文件末尾修复）
- check-yaml（YAML 检查）
- clang-format（用于 C/C++ 文件）

安装：`pip install pre-commit && pre-commit install`

---

## 3. 代码风格规范

### 版本号口径
双层口径：公共头（`include/zeplod/`）仅当公共 API 语义变化（增删改签名/返回值/可观测行为契约）时 `@version` +0.1；`src/` 内部实现文件（含 `*_internal.h`）按整改批次逐批 +0.1。行为不变的内部修改不升版本、不记日志。

### 总体规则
- **语言**：C（Zephyr RTOS）
- **标准**：C11（`<stdint.h>`、`<stdbool.h>` 等）
- **缩进**：4 空格（不使用 Tab）
- **列宽限制**：120 字符
- **换行符**：LF（Unix 风格）

### 文件头部模板
```c
/**
 * @file <filename>
 * @brief <简要描述>
 * @author zeh (china_qzh@163.com)
 * @version X.Y
 * @date YYYY-MM-DD
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 *  YYYY-MM-DD     X.Y            name           初始版本
 */
```

### 头文件保护宏
```c
#ifndef MODULE_NAME_H
#define MODULE_NAME_H
// ... 内容 ...
#endif /* MODULE_NAME_H */
```

### 头文件包含顺序
1. `<zephyr/...>`（Zephyr 头文件 - 最高优先级）
2. `<...>`（标准库头文件）
3. `<zeplod/...>`（框架对外 API，见 `include/zeplod/`）
4. `"..."`（本模块私有头文件或 `src/` 内部实现头文件，如 `*_internal.h`）

### 命名规范

| 类型 | 规范 | 示例 |
|------|------|------|
| 全局变量 | `g_<name>` | `g_event_system` |
| 静态变量 | `g_<name>` | `g_event_msgq_buffer` |
| 常量/宏 | `UPPER_SNAKE_CASE` | `CONFIG_EVENT_QUEUE_SIZE` |
| 函数 | `snake_case` | `event_system_init()` |
| 类型/结构体 | `snake_case_t` | `event_type_t`、`event_status_t` |
| 枚举值 | `PREFIX_VALUE` | `EVENT_PRIORITY_NORMAL` |
| 模块接口 | `DECLARE_MODULE_INTERFACE(name)` | `DECLARE_MODULE_INTERFACE(my_module)` |
| 模块函数 | `<name>_<action>` | `my_module_init()`、`my_module_start()` |

### 函数注释风格
```c
/**
 * @brief 简要描述
 *
 * 详细描述（如果需要）。
 *
 * @param param_name 参数描述
 * @return 返回值描述
 * @note 重要注意事项
 */
return_type function_name(param_type param_name) {
    // ...
}
```

### 大括号风格
```c
// K&R 风格 - 左大括号在同行
if (condition) {
    do_something();
} else {
    do_something_else();
}
```

### 错误处理
- 使用枚举错误码（如 `event_status_t`、`APP_ERR_*`）
- 返回负数 errno 风格值：`-EINVAL`、`-ENOMEM` 等
- 成功返回 `EVENT_OK`（0）
- 检查所有返回值

```c
event_status_t result = event_publish(&my_event);
if (result != EVENT_OK) {
    LOG_ERR("Failed to publish event: %d", result);
    return result;
}
```

### 创建新模块步骤

1. **定义模块优先级**（在 `include/zeplod/app_config.h`）：
```c
#define APP_INIT_PRIO_MODULE_MINE  60  // 在 MODULE_MGR(54) 和 APP_FINAL(99) 之间
```

2. **创建模块头文件**（`include/zeplod/my_module.h`）：
```c
#ifndef MY_MODULE_H
#define MY_MODULE_H

#include <zeplod/module_base.h>

typedef struct {
    uint32_t param1;
    uint32_t param2;
} my_module_config_t;

int my_module_init(void *config);
int my_module_start(void);
int my_module_stop(void);
int my_module_shutdown(void);
void my_module_on_event(const event_t *event, void *user_data);
module_status_t my_module_get_status(void);
int my_module_control(int cmd, void *arg);

DECLARE_MODULE_INTERFACE(my_module);

#endif
```

3. **实现模块**（`src/modules/my_module.c`）：
```c
#include <zeplod/my_module.h>
#include <zeplod/app_config.h>

static my_module_config_t g_config;

int my_module_init(void *config) {
    if (config != NULL) {
        g_config = *(my_module_config_t *)config;
    }
    return 0;
}

int my_module_start(void) {
    return 0;
}

int my_module_stop(void) {
    return 0;
}

int my_module_shutdown(void) {
    return 0;
}

void my_module_on_event(const event_t *event, void *user_data) {
    // 处理事件
}

module_status_t my_module_get_status(void) {
    return MODULE_STATUS_RUNNING;
}

int my_module_control(int cmd, void *arg) {
    return 0;
}

DECLARE_MODULE_INTERFACE(my_module);

// 注册模块
static int my_module_auto_register(void) {
    uint32_t id;
    return module_manager_register(&my_module_interface, &g_config, &id) ? -EIO : 0;
}
SYS_INIT(my_module_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_MINE);
```

4. **在 CMakeLists.txt 添加源文件**（按字母顺序添加到对应位置）

### 事件系统使用模式
```c
// 订阅事件
event_subscribe(EVENT_TYPE_SENSOR_DATA, my_callback, user_data, &subscriber_id);

// 订阅级过滤（须 CONFIG_EVENT_SUBSCRIBER_FILTER=y）：filter 在分发线程回调前判定
event_subscribe_filtered(EVENT_TYPE_SENSOR_DATA, my_callback, user_data, my_filter, &filter_ctx, &subscriber_id);

// 发布事件（推荐 publish_copy / create_with_data，勿手搓不一致的 flags/data_len）
event_publish_copy(EVENT_TYPE_SENSOR_DATA, EVENT_PRIORITY_NORMAL, &data, sizeof(data));

// ISR 中发布事件
event_publish_from_isr(&my_event);
```

**使用契约（发布 / 卸载 / 手动消费）**

| 场景 | 要求 |
|------|------|
| 发布 | 先 `event_register_type`；负载用 `event_create*` / `event_publish_copy`，`event_publish` 会校验 `flags` 与 `data_len` |
| 模块卸载 | `event_system_stop()` → `event_unsubscribe_all(id)` → 再释放 `user_data`（`unsubscribe` 返回后回调仍可能短暂执行） |
| 手动 `process_one` / `process_all` | 仅 init 后、首次 `event_dispatcher_start()` 之前；曾 start 后再 stop 或与 RUNNING 的分发器线程并行调用会返回 `EVENT_ERR_INVALID_ARG` |
| `CONFIG_EVENT_QUEUE_OVERFLOW_DROP_LOWEST` | 仅线程侧 `event_publish` 可挤掉低优先级；ISR 满队列仍为 `DROP_NEWEST` |
| 订阅级 filter（`CONFIG_EVENT_SUBSCRIBER_FILTER=y`） | 分发线程回调前判定；返回 false 仅跳过该订阅者（事件对其他订阅者照常分发）；与全局 dispatcher filter 独立叠加且全局先判；filter 回调内不得阻塞/发布/调订阅 API |
| Sticky 事件（`CONFIG_EVENT_STICKY=y`，默认 n） | 先 `event_sticky_enable(type)` 再发布；仅锁存启用后的线程侧发布最近一条沿（不回溯历史，ISR 发布不更新），新订阅者订阅成功时在订阅线程同步重放一次（不经过滤器、不计入 total_events）；锁存 best-effort（拷贝失败发布仍成功，计 miss）；未注册/槽池满返回 `EVENT_ERR_NOT_FOUND`；重放回调内订阅其他锁存类型不会递归重放（每线程深度防御） |
| 延迟/周期发布（`CONFIG_EVENT_TIMER_POOL=y`，默认 n；周期另须 `CONFIG_EVENT_PERIODIC_PUBLISH=y`） | 延迟 `event_publish_delayed_copy` 深拷贝负载入槽（上界 4KB），`delay_ms` 后到期回调（时钟 ISR 上下文）以槽内事件壳零分配投递；周期 `event_periodic_start` 母本入槽，每轮经纯 slab 路径生成新副本，绝不 `k_malloc`；到期投递尽力而为（队列满/未 running/副本分配失败该轮丢弃计 miss，不补发不重试）；句柄 slot+gen 防 ABA，取消与到期回调竞态由 gen 仲裁（回调已通过校验返回 `EVENT_ERR_NOT_FOUND` 不干扰）；`stop` 不清 armed（停止期间轮次丢弃计 miss，恢复继续），`shutdown` 取消全部并释放负载；提交/取消仅线程上下文，ISR 直接用 `event_publish_from_isr` |
| 优先级有序分发（`CONFIG_EVENT_PRIORITY_DISPATCH=y`，默认 n；与 `EVENT_QUEUE_OVERFLOW_BLOCK` 硬互斥） | 出队侧按「有效优先级」排序：`effective = max(priority − (now−timestamp)/CONFIG_EVENT_PRIORITY_AGING_STEP_MS, 0)`，数值小者先出、平局取 FIFO 最旧；仅出队侧重排（`event_queue_dequeue_priority_batch`），入队与 ISR 路径零改动。事件 `timestamp` 须为创建时刻（`event_create*`/`publish_copy*` 已满足），**手搓 `event_t` 必须设置 timestamp**，否则按严重老化退化为 FIFO（不乱序但失去优先级语义）。手动 `process_one`/`process_all` 开启后亦有序（行为变化点）；全局 dispatcher filter 在出队后应用，被拒高优事件仍占用一次最优选择；`event_notify_subscribers` 直发不参与排序（正交）；与 `DROP_LOWEST` 挤占共用同一 scratch，经 reorder_lock 互斥串行。饿死界 = 最大优先级差 × AGING_STEP + 一个出队周期（LOW(10) 与新 CRITICAL 平局需 10×AGING_STEP）。内存代价：每队列 scratch 1×容量×sizeof(event_t)（默认 4KB，init 预分配；与 DROP_LOWEST 组合时 2×）+ 分发器批缓冲 容量×sizeof(event_t)。与 BLOCK 互斥原因：BLOCK 在 `k_msgq_put` 内驻留发布者，优先级出队需在 reorder_lock 下排空/回灌队列，组合须入队两段式改造，保守起见 Kconfig 直接拒绝 |

**自动初始化（标准版，默认启用）**

| 阶段 | SYS_INIT 回调 | 优先级 |
|------|----------------|--------|
| 事件系统 init+start | `event_compat_auto_init` | `APP_INIT_PRIO_EVENT_SYS` (40) |
| 分发器 init+start | `event_dispatcher_auto_init` | `APP_INIT_PRIO_DISPATCHER` (45) |

- 链接了 autoinit 时，应用层**不要**再手动 `event_dispatcher_init()` / `event_dispatcher_start()`，以免重复创建分发线程。
- 单元测试若需独占队列消费者，应 `event_system_shutdown()` 收尾（见 `test_event_dispatcher.c`）。
- `CONFIG_EVENT_QUEUE_OVERFLOW_BLOCK=y` 时，`event_publish` 入队使用 `K_FOREVER` 阻塞等待空位；ISR 仍使用 `K_NO_WAIT`。

### 状态机与锁序（核心模块）

`src/core/state_machine.*` 与 `src/core/lock_order.*` 已接入 `event_system`、`module_manager`、`ipc_service`。新模块或多锁路径请遵循：

**状态机**

- 生命周期使用 `zepl_state_machine_t` + `zepl_state_machine_try_transition()`，状态集见 `include/zeplod/state_machine.h`（`ZEP_STATE_UNINIT` … `ZEP_STATE_ERROR`）。
- `event_system` **不使用** `ZEP_STATE_ERROR`，失败仍返回 `event_status_t`；`module_manager` / `ipc_service` 在 start/stop 路径会检查 ERROR。
- 可与 `initialized`、`running` 等原子/布尔并存：状态机管阶段，原子管热路径与 ISR，勿混用语义。

**锁序**

- 在 `k_mutex_lock` 之后、`k_mutex_unlock` 之前配对调用：

```c
zepl_lock_enter(ZEP_LOCK_LEVEL_TABLE, (uintptr_t)&my_lock);
k_mutex_lock(&my_lock, K_FOREVER);
/* ... */
k_mutex_unlock(&my_lock);
zepl_lock_exit(ZEP_LOCK_LEVEL_TABLE, (uintptr_t)&my_lock);
```

- 层级：`GLOBAL(1) → STATE(2) → TABLE(3) → ENTRY(4) → RESOURCE(5)`；同层多锁时 key（锁指针）须单调不减。
- 持锁期间不要调用可能回调、阻塞或再入本模块的 API；`module_manager` 退订须「锁内冻结、锁外 `event_unsubscribe_all`」。
- ztest 线程结束可调用 `zepl_lock_reset_current_thread()`。

详细决策与 PR 范围：`docs/superpowers/plan/2026-05-23-state-machine-lock-ordering-implementation-plan.md`。

---

## 4. 项目结构

```
zeplod/
├── APP_VERSION                 # 应用语义化版本（X.Y.Z）
├── CMakeLists.txt              # 构建配置
├── Kconfig                     # 应用 Kconfig（含事件/模块/IPC 等）
├── prj.conf                    # 默认 Zephyr 配置（最小配置）
├── conf/                       # 叠加配置片段（profiles/targets/features/examples）
├── app.overlay                 # 通用设备树覆盖
├── west.yml                   # West 清单
├── zephyr_config.env           # 本地路径配置（勿提交）
├── .clang-format              # 代码格式化配置
├── .clang-tidy                # 静态分析配置
├── .pre-commit-config.yaml    # pre-commit 钩子配置
├── boards/
│   └── overlay.dts            # 通用设备树覆盖
├── include/
│   └── zeplod/                # 对外公开 API 头文件（统一 #include <zeplod/...>）
│       ├── zeplod_framework.h # 伞头：事件系统 + 状态机 + 锁序
│       ├── zeplod_module.h    # 伞头：模块系统
│       ├── event_system.h
│       ├── module_manager.h
│       ├── app_config.h
│       └── ...
├── src/
│   ├── core/                  # 事件系统实现（内部头 *\_internal.h 等）
│   │   ├── event_system.c
│   │   ├── event_queue.c/h
│   │   ├── event_dispatcher.c
│   │   ├── event_sticky.c     # Sticky 锁存/重放（CONFIG_EVENT_STICKY，默认 n）
│   │   ├── event_timer.c/h    # 延迟/周期发布定时器池（CONFIG_EVENT_TIMER_POOL，默认 n）
│   │   ├── event_system_compat.c
│   │   ├── state_machine.c
│   │   └── lock_order.c
│   ├── services/              # 系统服务实现
│   │   ├── sys_log.c
│   │   ├── sys_memory.c
│   │   ├── sys_watchdog.c
│   │   └── sys_timer.c
│   ├── modules/               # 模块管理器与 IPC 实现
│   │   ├── module_manager.c
│   │   ├── module_manager_compat.c
│   │   └── ipc_service/
│   ├── modules_examples/      # 示例模块 .c（头文件在 include/zeplod/）
│   ├── data_bus/              # Data Bus 实现
│   ├── app/                   # 应用层实现
│   │   ├── app_main.c
│   │   ├── app_version.c
│   │   └── app_kv.c
├── tests/                    # ztest 单元测试
│   ├── CMakeLists.txt
│   ├── Kconfig
│   ├── prj.conf
│   ├── test_event_system.c
│   ├── test_event_queue.c
│   ├── test_event_dispatcher.c
│   ├── test_event_filter.c   # 订阅级过滤（叠加 prj_event_ext.conf）
│   ├── test_event_priority.c # 优先级有序分发（叠加 prj_event_ext.conf）
│   ├── test_event_sticky.c   # Sticky 锁存/重放（叠加 prj_event_ext.conf）
│   ├── test_event_timer.c    # 延迟/周期发布（叠加 prj_event_ext.conf）
│   ├── test_module_manager.c
│   ├── test_sys_memory.c
│   ├── test_sys_timer.c
│   ├── test_sys_watchdog.c
│   ├── test_sys_log.c
│   └── test_ipc_service.c
├── docs/                     # 文档
├── scripts/                   # 工具脚本
└── copywriting/              # 版权文件
```

---

## 5. 模块系统

### 模块生命周期
`init → start → run → stop → shutdown`

### 模块接口（module_base.h）
```c
typedef struct {
    const char*       name;
    uint32_t          version;
    module_priority_t priority;
    const char* const* depends_on;  // NULL 结尾的模块名数组
    int (*init)(void* config);
    int (*start)(void);
    int (*stop)(void);
    int (*shutdown)(void);
    module_event_handler_t on_event;
    module_status_t (*get_status)(void);
    int (*control)(int cmd, void* arg);
} module_interface_t;
```

### SYS_INIT 优先级（app_config.h）
数值越小越早执行，同级别按文件添加顺序：
- `APP_INIT_PRIO_APP_CB` = 10
- `APP_INIT_PRIO_APP_KV` = 11
- `APP_INIT_PRIO_SYS_LOG` = 20
- `APP_INIT_PRIO_SYS_MEM` = 30
- `APP_INIT_PRIO_SYS_DIAG` = 31
- `APP_INIT_PRIO_SYS_SECURE_KV` = 32
- `APP_INIT_PRIO_SYS_FAULT_DUMP` = 33
- `APP_INIT_PRIO_SYS_TIME` = 34
- `APP_INIT_PRIO_EVENT_SYS` = 40
- `APP_INIT_PRIO_DISPATCHER` = 45
- `APP_INIT_PRIO_SYS_TIMER` = 50
- `APP_INIT_PRIO_SYS_WDT` = 52
- `APP_INIT_PRIO_MODULE_MGR` = 54
- `APP_INIT_PRIO_MODULE_CONNECTIVITY` = 55
- `APP_INIT_PRIO_MODULE_PROVISIONING` = 56
- `APP_INIT_PRIO_MODULE_RECOVERY` = 57
- `APP_INIT_PRIO_MODULE_OTA` = 58
- `APP_INIT_PRIO_MODULE_FACTORY` = 59
- `APP_INIT_PRIO_MODULE_REMOTE_OPS` = 66
- `APP_INIT_PRIO_MODULE_FEATURE_GATE` = 67
- `APP_INIT_PRIO_MODULE_A` = 60
- `APP_INIT_PRIO_MODULE_B` = 61
- `APP_INIT_PRIO_MODULE_GPIO` = 62
- `APP_INIT_PRIO_MODULE_UART` = 63
- `APP_INIT_PRIO_MODULE_IPC` = 64
- `APP_INIT_PRIO_MODULE_MULTI` = 65
- `APP_INIT_PRIO_APP_FINAL` = 99

---

## 6. 配置（Kconfig）

- **位置**：根目录 `Kconfig` + `src/modules/ipc_service/Kconfig`
- **配置文件**：`prj.conf`（基线）、`conf/` 下叠加片段（见 `conf/README.md`）、`tests/prj_*.conf`（测试专用）
- **设备树**：`boards/overlay.dts`、`boards/<board>.overlay`、`app.overlay`

### 常用 Kconfig 选项
```
CONFIG_EVENT_SYSTEM=y
CONFIG_EVENT_QUEUE_SIZE=64
CONFIG_EVENT_MAX_SUBSCRIBERS=16
CONFIG_EVENT_DISPATCHER_STACK_SIZE=2048
CONFIG_EVENT_DISPATCHER_PRIORITY=5

CONFIG_MODULE_MANAGER=y
CONFIG_MAX_MODULES=16
CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES=n

CONFIG_APP_ENABLE_SHELL=y
CONFIG_APP_ENABLE_TOP=y

CONFIG_SYS_LOG_LEVEL=3
CONFIG_SYS_MEMORY_POOL_SIZE=8192
CONFIG_SYS_WATCHDOG_ENABLE=y
CONFIG_SYS_WATCHDOG_TIMEOUT_MS=5000

# 产品化能力（默认关闭，见 conf/features/）
# CONFIG_OTA_MODULE=y
# CONFIG_SYS_DIAG_ENABLE=y
# CONFIG_RECOVERY_POLICY_MODULE=y
# CONFIG_SYS_SECURE_KV_ENABLE=y
# CONFIG_SYS_FAULT_DUMP_ENABLE=y
# CONFIG_SYS_TIME_ENABLE=y
# CONFIG_CONNECTIVITY_MODULE=y
# CONFIG_PROVISIONING_MODULE=y
# conf/features/net_stub.conf — 三合一
# CONFIG_FACTORY_MODE_MODULE=y
# CONFIG_REMOTE_OPS_MODULE=y
# CONFIG_FEATURE_GATE_MODULE=y
# conf/features/scale.conf
# conf/profiles/sku_gateway.conf
# conf/targets/production.conf — 量产叠加
# MCUboot OTA：conf/targets/mcuboot.conf

# 启动日志：默认仅 Banner（CONFIG_APP_ENABLE_BANNER）+ 少量 INF；详版版本块需打开：
# CONFIG_APP_BOOT_VERBOSE=y
```

---

## 7. 持续集成（CI）

### GitHub Actions（`.github/workflows/ci.yml`）
- 代码质量：ShellCheck、pre-commit
- 构建：native_posix（CI Zephyr 3.6）、ARM 板矩阵；本机 Zephyr 4.x 推荐 native_sim
- 测试：native_posix ztest（CI）；本机可用 `scripts/run_tests.sh`
- 文档：Doxygen 生成

### GitLab CI（`.gitlab-ci.yml`）
GitHub Actions 的镜像。

---

## 8. 提交信息格式（Conventional Commits）

```
<type>(<scope>): <subject>

<body>

<footer>
```

### 类型说明
| 类型 | 说明 |
|------|------|
| `feat` | 新功能 |
| `fix` | Bug 修复 |
| `docs` | 文档变更 |
| `style` | 代码格式（不改逻辑） |
| `refactor` | 重构 |
| `perf` | 性能优化 |
| `test` | 测试相关 |
| `build` | 构建系统 |
| `ci` | CI/CD |
| `chore` | 其他 |

### 示例
```bash
git commit -m "feat(event): 增加 EVENT_TYPE_SENSOR_DATA 类型"
git commit -m "fix(module): 修复模块启动顺序错误"
git commit -m "docs(ci): 更新 CI 板型配置说明"
```

---

## 9. 关键 Zephyr API

- `k_msgq` - 消息队列
- `k_mutex` - 互斥锁
- `k_timer` - 定时器
- `LOG_MODULE_REGISTER()` - 日志模块注册
- `SYS_INIT()` - 初始化
- `DEVICE_DT_GET()` - 设备树
- `k_malloc()` / `k_free()` - Zephyr 内存分配

---

## 10. 重要注意事项

1. **禁止使用类型压制** - 这是 C 语言，不是 TypeScript
2. **必须检查返回值** - 错误处理是强制要求
3. **使用 Zephyr 内存分配** - 使用 `k_malloc()`/`k_free()` 而不是标准库
4. **线程安全** - 共享数据使用互斥锁/原子操作
5. **ISR 安全函数** - 在中断上下文使用 `event_publish_from_isr()`
6. **模块隔离** - 模块间通过事件通信，不使用直接调用
7. **版本文件** - 使用 `APP_VERSION`（不要用 `VERSION`，与 Zephyr 冲突）
