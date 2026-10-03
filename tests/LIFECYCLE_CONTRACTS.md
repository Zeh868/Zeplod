# 核心模块生命周期与并发边界契约

> 与 [核心架构重构路线图](../docs/zh-CN/20-架构设计/27-核心架构重构路线图.md) P0/P2 对齐。
> 线程服务对照表见 [线程服务生命周期约定](../docs/zh-CN/30-核心模块/38-线程服务生命周期约定.md)。
> 日期：2026-05-28

本文档固化 **测试应断言的返回值语义**；实现变更须先更新本文档与对应用例。

## event_system

| 场景 | 预期 |
| --- | --- |
| 未 `init` 调用 `start` / `publish` | `EVENT_ERR_INVALID_ARG` 或 `EVENT_ERR_NOT_RUNNING`（见具体 API） |
| 未 `start` 时 `publish` | `EVENT_ERR_NOT_RUNNING` |
| 重复 `init` | `EVENT_OK`（幂等） |
| 重复 `start`（已 RUNNING） | `EVENT_OK` |
| 重复 `stop` | `EVENT_OK` |
| `STOPPED` 后再次 `start` | `EVENT_OK`，`is_running()==true` |
| 重复 `shutdown` | `EVENT_OK` |
| 从 dispatcher 线程 `stop` / `shutdown` | `EVENT_ERR_INVALID_ARG` |
| 订阅未注册类型 | `EVENT_ERR_NOT_FOUND` |
| 空回调订阅 | `EVENT_ERR_INVALID_ARG` |

## event_dispatcher

| 场景 | 预期 |
| --- | --- |
| 未 `init` 时 `start` | `EVENT_ERR_INVALID_ARG` |
| 重复 `start`（已 RUNNING） | `EVENT_OK` |
| 重复 `stop`（已 STOPPED 且 join 完成） | `EVENT_OK` |
| `ever_started` 后手动 `process_one` | `EVENT_ERR_INVALID_ARG` |
| 从 dispatcher 线程 `stop` / `deinit` | `EVENT_ERR_INVALID_ARG` |
| join 超时 | `EVENT_ERR_TIMEOUT`（`thread_started` 仍为 true，可重试 stop） |

## ipc_service

| 场景 | 预期 |
| --- | --- |
| `init` 参数非法（NULL service/name/func） | `-EINVAL` |
| 未 `start` 时 `ipc_call_sync` / `ipc_call_async` | `-EINVAL`（`ipc_service_is_accepting_requests` 为 false） |
| 未 `start` 时 `ipc_service_stop` | `0`（幂等） |
| 重复 `start`（已 RUNNING） | `-EALREADY` |
| 重复 `stop`（已停止） | `0` |
| `stop` 后再次 `start` | `0`（见 `test_multiple_start_stop_cycles`） |
| 从 worker 或 dispatcher 线程 `stop` | `-EDEADLK` |
| `stop` 时未完成 future | future 完成，结果 `-ECANCELED` |
| `cancel` 已完成 SYNC 请求 | `-EALREADY` |
| `cancel` 不存在的 request_id | `-ENOENT` |
| join 超时 | `-EIO`，保持 `ZEP_STATE_STOPPING` 和资源；线程退出后可重试 `stop` |

## data_bus

| 场景 | 预期 |
| --- | --- |
| 重复 `data_bus_init` | `0` |
| `shutting_down` 期间 `data_bus_init` | `-ESHUTDOWN` |
| 未 init 时 `data_bus_channel_create` 等 | `-ENODEV` |
| 重复 `data_bus_deinit` | `0` |
| 从 dispatcher 线程 `data_bus_deinit` | `-EINVAL`（见 `test_deinit_rejected_from_dispatcher_thread`） |
| 分发线程 join 超时 | `-EIO`（`g_initialized` / `g_shutting_down` 仍为 1，**勿**再 `init`，应重试 `deinit`） |
| 通道操作 drain 超时 | `-EBUSY`（dispatcher 已停止，保持 `shutting_down=1`，拒绝 publish/init，应重试 `deinit`） |
| 从 dispatcher 回调注销 consumer | `-EDEADLK` |
| `shutting_down` 期间 publish | `-ESHUTDOWN`（经 `data_bus_require_initialized`） |

## 测试编写约定

- 异步完成优先使用 `ztest_sync.h` 中的 `ztest_wait_atomic_*` 或 `k_sem` / `k_event`。
- 禁止将 `(event_callback_t)0x1000` 等不可调用地址作为有效订阅回调。
- 生命周期循环测试至少覆盖：`init → start → stop → (可选再次 start) → shutdown/deinit`。
- IPC/Data Bus 边界用例命名与本文档表格场景一一对应，便于回归。

## 相关测试套件（按配置）

| 配置 | 套件 |
| --- | --- |
| `prj.conf` | 核心 event、module_manager、data_bus（**硬件默认**） |
| `prj.conf;prj_test_extensions.conf` | 上列 + P0/P1 扩展 + `test_concurrency_stress`（**native_sim CI 默认**） |
| `prj.conf;prj_concurrency_stress.conf` | 上列 + 仅 `test_concurrency_stress`（硬件 640KB 后可选） |
| `prj.conf;prj_block_overflow.conf` | 上列 + `test_block_publish_unblocks_on_stop` |
| `prj.conf;prj_native_sim.conf` | 上列 + `test_ipc_service` + 扩展项 |
| `prj.conf;prj_native_sim.conf;prj_ci_examples.conf` | 上列 + `test_example_module_*` |
