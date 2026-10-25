# AGENTS.md - Zeplod AI 编码代理规则

本文件面向在本仓库中工作的 AI 编码代理，只保留可执行约束。面向人的协作说明见
`docs/zh-CN/80-贡献与维护/83-AI协作开发指南.md`；项目技术文档总入口见
`docs/zh-CN/00-入门/02-文档索引.md`。

## 1. 事实源与工作边界

发生冲突时按以下顺序判断：

1. 当前代码、`Kconfig`、`CMakeLists.txt`、`.github/workflows/ci.yml`
2. 对应主题的 canonical 文档（见 `docs/zh-CN/00-入门/05-任务导航.md`）
3. README、示例、路线图和历史设计稿

- 修改前先执行 `git status --short`；用户已有改动不得覆盖、回退或顺手格式化。
- 默认只做请求范围内的最小改动。未经明确授权，不得 commit、push、改 CI 行为、改许可证或删除用户文件。
- 不得提交密钥、Token、证书、本机绝对路径或 `zephyr_config.env`。
- 对硬件行为、ISR 安全性、Zephyr 版本差异不确定时，先查代码/官方契约并报告不确定性，不得猜测。

## 2. 模型编排与任务包

采用能力角色而不是把流程绑定到长期固定的模型名称：

- **主控**：理解需求、拆分、决策、审查、验收、向用户汇报。
- **实现代理**：实现、批量修改、构建测试、失败后修错闭环。
- **审查代理**：只读检查代码、diff、文档和风险；除非任务明确要求，不直接修改。

具体模型名称属于调用端配置。更换模型时不得改变本文件中的质量门槛与验收证据要求。

委派实现任务时使用完整任务包：

- **目标**：一句话描述可验收结果
- **范围**：允许修改路径与禁止区域
- **约束**：风格、Doxygen、安全与 Git 限制
- **上下文**：关键文件、接口、错误原文
- **验收**：实际可执行的命令或检查项
- **返回**：摘要、改动文件、验证（命令与证据）、未决项

代理不得把“计划执行”“理论上通过”写成“已验证”。未执行的检查必须明确标为未运行并说明原因。

## 3. 当前工具链与常用命令

前置：Zephyr 4.3.x、兼容的 Zephyr SDK、CMake 3.20+、West、满足该 Zephyr 发布要求的 Python。
本仓库 CI 当前使用 **Zephyr 4.3.0 + `native_sim`**。`native_sim` / `native_posix`
仅用于 Linux/WSL2；Windows 原生和 macOS 使用 `qemu_riscv32`。

```bash
# Linux/macOS/WSL2：仅激活环境
source scripts/setup_env.sh

# Linux/WSL2：主应用主机仿真
west build -b native_sim .

# 自动选板并运行测试（native_sim 优先）
./scripts/run_tests.sh

# 手动运行默认测试矩阵
west build -b native_sim tests/ --build-dir build_tests -p always -- \
  -DCONF_FILE="prj.conf;prj_test_extensions.conf;prj_event_ext.conf"
west build -t run --build-dir build_tests

# 全量 IPC + Slab
west build -b native_sim tests/ --build-dir build_tests_ipc -p always -- \
  -DCONF_FILE="prj.conf;prj_native_sim.conf"
west build -t run --build-dir build_tests_ipc
```

```powershell
# Windows 原生：脚本自动选择 qemu_riscv32
.\scripts\run_tests.ps1
```

硬件板测试、覆盖率、Sanitizer、Twister 与 QEMU 命令以 `tests/README.md` 和
`docs/zh-CN/60-调试与排错/63-脚本与工具说明.md` 为准。

## 4. 变更类型与最低验证

| 变更 | 最低验证 |
|---|---|
| C/H | 对修改文件执行 clang-format 检查；构建受影响配置；运行相关 ztest |
| 公共 API (`include/zeplod/`) | 上述检查 + Doxygen；核对兼容性与 `@version` 口径 |
| Kconfig/CMake/conf | 至少构建默认配置和直接受影响的叠加配置 |
| 测试 | 运行对应 suite；异步测试遵循 `tests/LIFECYCLE_CONTRACTS.md` |
| 文档 | 运行下列文档检查；涉及 API 时运行 Doxygen |
| 脚本 | 运行脚本自身帮助/静态检查及其最小成功路径 |

```bash
python scripts/lint_docs.py docs/zh-CN --skip-config-check \
  --check-index --check-code-map --check-parity --strict
python scripts/lint_docs.py docs --skip-config-check --check-residual-chinese --strict
python scripts/lint_docs.py tests --skip-config-check --strict
python scripts/check_script_docs.py
python scripts/check_encoding.py
python scripts/doc_inventory.py --strict
doxygen Doxyfile
git diff --check
```

## 5. C 代码与 Doxygen

- C11，4 空格，LF，列宽 120，K&R 大括号；遵循根目录 `.clang-format`。
- 包含顺序：`<zephyr/...>` → 标准库 → `<zeplod/...>` → 本模块私有头。
- 全局/静态变量使用 `g_`；函数用 `snake_case`；类型用 `snake_case_t`；宏用 `UPPER_SNAKE_CASE`。
- 必须检查返回值；使用项目错误码或负 errno；使用 Zephyr 内存 API。
- C/H 文件头、公共 API、`static` 辅助函数使用中文 Doxygen 注释。
- 公共头仅在 API 语义变化时将 `@version` +0.1；内部实现按整改批次 +0.1。行为不变的内部修改不升版本。
- 应用版本唯一源为根目录 `APP_VERSION`，不要创建 `VERSION`。

完整风格和贡献规范见 `docs/zh-CN/80-贡献与维护/81-参与贡献与代码规范.md`。

## 6. 事件系统强约束

- 发布前先 `event_register_type`。
- 有负载时优先使用 `event_create*` / `event_publish_copy*`，不要手工组合不一致的 `flags` 与 `data_len`。
- ISR 使用 `event_publish_from_isr()`；不得在 ISR 调用会阻塞、取 mutex 或分配内核堆的路径。
- 模块卸载顺序：`event_system_stop()` → `event_unsubscribe_all(id)` → 释放 `user_data`。取消订阅返回后，已快照回调仍可能短暂执行。
- `process_one` / `process_all` 仅允许在 init 后、分发器第一次 start 前使用；曾 start 后再 stop 也不得手动消费。
- `DROP_LOWEST` 只改变线程侧发布；ISR 满队列仍按 `DROP_NEWEST`。
- 订阅 filter 在分发回调前执行，不得阻塞、发布事件或调用订阅 API；全局 dispatcher filter 先于订阅 filter。
- Sticky 重放在订阅线程同步发生，不经过滤器、不计入 total_events；ISR 发布不更新锁存。
- 延迟/周期发布的提交和取消仅限线程上下文；到期投递为 best-effort，不补发、不重试。
- 开启优先级分发时，手工构造 `event_t` 必须设置创建时刻 `timestamp`；该功能与 BLOCK 溢出策略互斥。

完整契约见 `docs/zh-CN/30-核心模块/31-事件系统详细使用说明.md`。

## 7. 状态机、锁序与线程服务

- 生命周期使用 `zepl_state_machine_t` 和 `zepl_state_machine_try_transition()`。
- 状态机管理阶段，原子/布尔只管理热路径；不得混用语义。
- 锁层级固定为 `GLOBAL(1) → STATE(2) → TABLE(3) → ENTRY(4) → RESOURCE(5)`。
- 同层多锁按锁指针 key 单调不减；`zepl_lock_enter/exit` 必须与实际加锁/解锁成对。
- 持锁期间不得调用外部回调、可能阻塞的 API 或可重入本模块的 API。
- `module_manager` 退订遵循“锁内冻结、锁外 `event_unsubscribe_all`”。
- 线程 stop/shutdown、重复 start/stop、取消和超时语义以
  `docs/zh-CN/30-核心模块/38-线程服务生命周期约定.md` 与
  `tests/LIFECYCLE_CONTRACTS.md` 为准。

## 8. 模块与配置

- 模块生命周期：`init → start → run → stop → shutdown`。
- 模块间优先通过事件或明确的公共抽象通信，不直接依赖另一模块的私有实现。
- 新模块的公开头放 `include/zeplod/`，实现放对应 `src/` 子目录；在本模块 `.c` 中使用 `SYS_INIT` 注册。
- 初始化优先级必须使用 `include/zeplod/app_config.h` 中的 `APP_INIT_PRIO_*`，不得散落魔数。
- 新增源码时同步模块 `CMakeLists.txt`、Kconfig、测试和代码文档映射。
- 配置说明以根 `Kconfig`、子目录 Kconfig、`prj.conf` 与 `conf/README.md` 为准，不在文档中猜默认值。

## 9. 完成与回传

完成前逐项检查：目标对齐、范围合规、质量/风险、Doxygen/风格、验收证据、未决项。
结论只使用：**通过**、**有条件通过**、**不通过**。

回传固定为：

- **摘要**
- **改动文件**
- **验证**（实际命令、退出码、关键证据）
- **未决项**（只列需要用户决策的事项）

## Cursor Cloud specific instructions

云端虚拟机已安装 Zephyr **v4.3.0** 与 Zephyr SDK **0.17.4**，不在本仓库内：

- `ZEPHYR_BASE=/opt/zephyrproject/zephyr`
- `ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-0.17.4`
- Python 虚拟环境 `/opt/zephyr-venv`，`west` 在 `/usr/local/bin`
- 登录 shell 与交互 bash 会加载 `/etc/zephyr.env`。非登录 shell 先执行 `source /etc/zephyr.env`

每次启动会把已跟踪的 `zephyr_config.env` 改写成上述 Linux 路径。该文件的仓库副本是维护者 Windows 机器的路径（本机工程目录为 `D:\Projects\shared\Zeplod`）。改写后 `git status` 会显示它被修改，不要提交。

不要在本仓库执行 `west update`。`west.yml` 中的 Zephyr URL 是开发者本机的 `file://D:/Code/...`。

`native_sim` 以 `-m32` 编译，环境已安装 `gcc-multilib` / `g++-multilib` / `libc6-dev-i386`。

主机仿真：

```bash
source /etc/zephyr.env
source scripts/setup_env.sh
west build -b native_sim .
west build -t run
```

`zephyr.exe` 会停在 shell。日志中的 `uart connected to pseudotty:` 才是 shell 的 TTY。可在该 TTY 上执行 `app status`、`app modules`、`app events`、`version`。

测试脚本在 Git 中没有可执行位，并且在 `set -o pipefail` 下 `west boards | grep -q` 会因 SIGPIPE 误选已删除的 `native_posix`。环境已导出 `ZEPHYR_TEST_BOARD=native_sim`。运行：

```bash
source /etc/zephyr.env
bash scripts/run_tests.sh
```
