> Language: **English** | [中文](../../zh-CN/00-入门/05-任务导航.md)

# Task Navigation

Find docs by **what you want to do**. Canonical docs hold the full detail; this page routes tasks only (≤2 clicks to the main doc).

Full index and learning paths: **[02-doc-index.md](02-doc-index.md)**. Code-to-doc mapping: **[06-code-doc-map.md](06-code-doc-map.md)**.

---

## Find by task

| I want to… | Read first | Then |
|------------|------------|------|
| Create a product APP repo (framework submodule) | [15 §3](../10-environment-build/15-creating-new-app-guide.md) | [11](../10-environment-build/11-environment-setup.md) → [14](../10-environment-build/14-qemu-simulation-guide.md) → [04](04-developer-guide.md) |
| First build of zeplod itself | [11](../10-environment-build/11-environment-setup.md) | [12](../10-environment-build/12-freestanding-app-build.md) → [04](04-developer-guide.md) |
| Run on Windows without hardware | [14](../10-environment-build/14-qemu-simulation-guide.md) | [63](../60-debugging/63-scripts-and-tools.md) |
| Run unit tests (Windows) | [14 §6](../10-environment-build/14-qemu-simulation-guide.md) | [51](../50-testing-ci/51-unit-testing-ci.md) → `tests/README.md` |
| Port to another board | [13](../10-environment-build/13-board-porting-guide.md) | [44](../40-app-development/44-devicetree-memory-config.md) → [43](../40-app-development/43-config-comparison-guide.md) |
| RAM / link overflow | [43](../40-app-development/43-config-comparison-guide.md) | [44](../40-app-development/44-devicetree-memory-config.md) |
| CONFIG_* meaning | [42](../40-app-development/42-config-options.md) | root `prj.conf` |
| Add a business module | [04](04-developer-guide.md) | [32](../30-core-modules/32-module-system-guide.md) → [31](../30-core-modules/31-event-system-guide.md) |
| Use Data Bus | [37](../30-core-modules/37-data-bus-guide.md) | [31](../30-core-modules/31-event-system-guide.md) |
| Use Thread IPC | [33](../30-core-modules/33-thread-ipc-service-guide.md) | [34](../30-core-modules/34-thread-ipc-integration-guide.md) |
| Flash and serial | [61](../60-debugging/61-flash-debug-quickstart.md) | [62](../60-debugging/62-troubleshooting.md) |
| Scripts / env vars | [63](../60-debugging/63-scripts-and-tools.md) | [06](06-code-doc-map.md) |
| Release / versioning | [71](../70-release-productization/71-version-management.md) | [73](../70-release-productization/73-release-checklist.md) |
| Troubleshoot | [62](../60-debugging/62-troubleshooting.md) | search by error in 62 |
| Environment / SDK setup | [11](../10-environment-build/11-environment-setup.md) | [12](../10-environment-build/12-freestanding-app-build.md) |
| Freestanding / BOARD_ROOT | [12](../10-environment-build/12-freestanding-app-build.md) | [44](../40-app-development/44-devicetree-memory-config.md) |
| Contribute / PR | [81](../80-contributing/81-contributing-code-style.md) | [82](../80-contributing/82-doc-improvements.md) |
| Use AI / coding agents | [83](../80-contributing/83-ai-collaboration-guide.md) | Root `AGENTS.md` |

---

## Canonical docs

| Topic | Canonical doc |
|-------|----------------|
| Environment / SDK / `zephyr_config.env` | [11-environment-setup](../10-environment-build/11-environment-setup.md) |
| Freestanding / `ZEPHYR_BASE` / BOARD_ROOT | [12-freestanding-app-build](../10-environment-build/12-freestanding-app-build.md) |
| New APP repo | [15-creating-new-app-guide](../10-environment-build/15-creating-new-app-guide.md) |
| QEMU simulation | [14-qemu-simulation-guide](../10-environment-build/14-qemu-simulation-guide.md) |
| Scripts / app vs framework layout | [63-scripts-and-tools](../60-debugging/63-scripts-and-tools.md) |
| Devicetree / overlay / memory | [44-devicetree-memory-config](../40-app-development/44-devicetree-memory-config.md) |
| Troubleshooting | [62-troubleshooting](../60-debugging/62-troubleshooting.md) |

Other docs should link here with a one-liner instead of duplicating long sections.
