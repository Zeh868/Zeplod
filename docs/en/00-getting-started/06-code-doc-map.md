> Language: **English** | [中文](../../zh-CN/00-入门/06-代码与文档映射.md)

# Code–Documentation Map

Single source of truth for **code path ↔ doc** links. When you change `scripts/`, `cmake/`, public Kconfig, or core `src/`, update the related doc and this table.

Task entry: [05-task-navigation.md](05-task-navigation.md) · Full index: [02-doc-index.md](02-doc-index.md)

---

## Mapping

| Code path | Documentation | Notes |
|-----------|---------------|-------|
| `scripts/project_layout.ps1` / `.sh` / `.py` | [63 §2](../60-debugging/63-scripts-and-tools.md), [15 §5](../10-environment-build/15-creating-new-app-guide.md) | app/framework layout, `Get-ZephyrQemuConfFile`, `Set-ZephyrConsoleUtf8` |
| `scripts/run_qemu.ps1` | [14](../10-environment-build/14-qemu-simulation-guide.md), [63 §3.2](../60-debugging/63-scripts-and-tools.md) | `-Board`, `-BuildOnly`, `-Target` |
| `scripts/run_tests.ps1` | [14 §6](../10-environment-build/14-qemu-simulation-guide.md), [51](../50-testing-ci/51-unit-testing-ci.md) | env vars, QEMU tests |
| `cmake/zeplod_app_overlays.cmake` | [15 §6](../10-environment-build/15-creating-new-app-guide.md), [44 overlay](../40-app-development/44-devicetree-memory-config.md) | skip `app.overlay` on QEMU |
| `zephyr_app.env.template` | [15 §3.7](../10-environment-build/15-creating-new-app-guide.md), [63 §6](../60-debugging/63-scripts-and-tools.md) | `APP_PRJ_CONF`, `QEMU_CONF` |
| `zephyr_config.env.template` | [11](../10-environment-build/11-environment-setup.md) | `ZEPHYR_BASE`, `QEMU_BIN_PATH` |
| `include/zeplod/` | [31](../30-core-modules/31-event-system-guide.md), [32](../30-core-modules/32-module-system-guide.md), [04](04-developer-guide.md) | Public API headers (`#include <zeplod/...>`) |
| `src/core/` | [31](../30-core-modules/31-event-system-guide.md), [23](../20-architecture/23-framework-internals.md) | event_system implementation |
| `src/modules/` | [32](../30-core-modules/32-module-system-guide.md) | module_manager |
| `src/modules_examples/` | [32](../30-core-modules/32-module-system-guide.md), [04](04-developer-guide.md) | Example modules (GPIO / UART / IPC…) |
| `src/modules/ipc_service/` | [33](../30-core-modules/33-thread-ipc-service-guide.md), [34](../30-core-modules/34-thread-ipc-integration-guide.md) | Thread IPC |
| `src/data_bus/` | [37](../30-core-modules/37-data-bus-guide.md) | Data Bus |
| `src/services/` | [36](../30-core-modules/36-system-services-guide.md) | sys_* services |
| `src/app/` | [04](04-developer-guide.md), [41](../40-app-development/41-zephyr-app-development.md) | app_main, shell |
| `boards/*.overlay`, `boards/overlay.dts` | [44](../40-app-development/44-devicetree-memory-config.md), [14](../10-environment-build/14-qemu-simulation-guide.md) | QEMU board overlays |
| `prj.conf` / `conf/` fragments | [42](../40-app-development/42-config-options.md), [14](../10-environment-build/14-qemu-simulation-guide.md), [15](../10-environment-build/15-creating-new-app-guide.md) | Kconfig fragments |
| `tests/` | [51](../50-testing-ci/51-unit-testing-ci.md), `tests/README.md` | ztest, Twister |

---

## Maintenance

1. PRs that touch any path above must verify the linked doc sections.
2. Add a row here before writing a new canonical doc section for new public scripts or CMake modules.
3. Run `python scripts/lint_docs.py --check-code-map` to verify paths exist (rows with `—` are skipped).
