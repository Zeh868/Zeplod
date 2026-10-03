> Language: [中文](../../zh-CN/80-贡献与维护/81-参与贡献与代码规范.md) | **English**

# Contributing and Code Standards

This document explains how to **submit changes** (Issue / Pull Request) to this repository, as well as expectations for **code style, linting tools, and CI**, so that contributors and reviewers are aligned.

**Related**: Root **[README.md](../../../README.md)** "Contributing" section · **[Unit Testing and CI Guide.md](../50-testing-ci/51-unit-testing-ci.md)** · **[Zephyr Version and CI Guide.md](../70-release-productization/72-zephyr-version-ci.md)**

---

## 1. Collaboration Workflow (Recommended)

1. **Fork** this repository to your namespace.  
2. **Create a feature branch** from **`main`** (or the project's default branch); naming suggestions: `feature/description`, `fix/description`.  
3. Complete development and **self-testing** on the branch (see "Pre-commit Checklist" below).  
4. Submit a **Pull Request (PR)**: use a **complete sentence** in the title to explain "what changed and why"; the body may include: related Issue, testing approach, breaking change description.  
5. Revise based on reviewer feedback; **keep commit history clean** (rebase / squash as per maintainer requirements).

For large changes, open an **Issue** first to discuss direction and avoid conflicts with maintainer expectations.

---

## 2. Pre-commit Checklist

| Item | Description |
|------|-------------|
| **Build** | At minimum **`west build -b <your-target-board> .`** passes; for common-code changes, also build **`native_sim`** + **`tests/`** on Linux/WSL (see **`tests/README.md`**). Use QEMU on native Windows. |
| **Format** | C code conforms to repository **`.clang-format`** (see next section). |
| **Test** | Related **ztest** cases pass; new logic should ideally include test coverage. |
| **Documentation** | If changing `Kconfig`, public APIs, or behavior, update **`docs/`** accordingly (at minimum the **[Project Configuration Guide.md](../40-app-development/42-config-options.md)** or the relevant topic guide) and necessary comments. |

For documentation changes, run the commands listed in the Chinese canonical contribution guide and the CI workflow, including strict whole-tree link and language checks.

---

## 3. Code Style and Static Analysis

- **Formatting**: Use **clang-format** with the style file at repository root **`.clang-format`**.  
  Example: `clang-format -i src/path/to/file.c`  
- **Static Analysis (optional)**: **`.clang-tidy`** can be used with **`compile_commands.json`** via IDE or command line (see the **[Developer Getting Started Guide.md](../00-getting-started/04-developer-guide.md)**).
  - **Pre-commit Hook (optional)**: If using **pre-commit**, configuration is in **`.pre-commit-config.yaml`** (install: `pip install pre-commit && pre-commit install`).

---

## 3.5 Commit Message Format Specification

This repository uses **Conventional Commits** style commit format, ensuring clear, traceable commit history and facilitating automatic changelog generation.

### 3.5.1 Basic Format

```
<type>(<scope>): <subject>

<body>

<footer>
```

**Single-line commit example**:
```bash
git commit -m "feat(sys_memory): add heap leak detection"
```

**Multi-line commit example**:
```bash
git commit -m "feat(event_system): optimize event queue performance

- Add queue overflow statistics
- Improve event drop logging
- Add high water mark monitoring

Closes: #123"
```

### 3.5.2 `<type>` Specification (Required)

| Type | Description | When to Use |
|------|-------------|-------------|
| **`feat`** | New feature | Add new feature, interface, or module |
| **`fix`** | Bug fix | Fix incorrect behavior or logic defects |
| **`docs`** | Documentation change | Modify README, manuals, comments (no logic change) |
| **`style`** | Code formatting | Formatting, whitespace, indentation (no logic change) |
| **`refactor`** | Refactoring | Code structure changes without changing functionality or API |
| **`perf`** | Performance optimization | Improve performance, reduce memory/CPU usage |
| **`test`** | Test related | Add/modify test cases, test configuration |
| **`build`** | Build system | Modify CMakeLists.txt, Kconfig, CI configuration |
| **`ci`** | CI/CD | Modify GitHub Actions, GitLab CI scripts |
| **`chore`** | Other chores | Version bumps, dependency updates, tool scripts, etc. |

**Discouraged types**: ~~`update`~~, ~~`modify`~~, ~~`change`~~, ~~`apply`~~ (too vague)

### 3.5.3 `<scope>` Scope (Optional but Recommended)

Indicates the affected area of the change for quick localization:

| Scope | Description | Example |
|-------|-------------|---------|
| **`event`** | Event system core | `event_system`, `event_queue`, `event_dispatcher` |
| **`module`** | Module manager | `module_manager`, business modules |
| **`service`** | System services | `sys_log`, `sys_memory`, `sys_timer`, `sys_watchdog` |
| **`ipc`** | Thread IPC service | `ipc_service` |
| **`app`** | Application main logic | `app_main`, `app_config`, `app_version` |
| **`board`** | Board support | `boards/`, device tree overlay |
| **`ci`** | Continuous integration | GitHub Actions, GitLab CI |
| **`docs`** | Documentation | Manuals under `docs/` |
| **`kconfig`** | Kconfig configuration | `Kconfig`, `prj.conf`, `prj_*.conf` |

**Examples**:
```
feat(event): add EVENT_TYPE_SENSOR_DATA type
fix(module): fix module startup order issue
docs(ci): update CI board configuration documentation
```

### 3.5.4 `<subject>` Title (Required)

**Rules**:
- ✅ Use **imperative mood** present tense ("add" not "added"/"adds")
- ✅ Start with **lowercase** (unless it's a proper noun)
- ✅ **No period** at the end
- ✅ Length ≤50 characters (Git standard)
- ✅ Keep commit subjects concise and omit filler words.

**Good examples**:
```
feat(sys_memory): add heap leak detection
fix(event): fix event queue statistics error
docs(app): update app_kv usage example
```

**Bad examples**:
```
change                        # Too vague
fix bug                       # No scope
add feature                   # No indication of what feature
update code                   # No information
feat: add new feature         # No scope, unclear
FIX: critical fix!            # Uppercase, exclamation mark
```

### 3.5.5 `<body>` Body (Optional)

**Multi-line motivation explanation** (suitable for complex changes):
- Explain **why** the change is needed (not "what was done")
- Each line ≤72 characters
- Briefly describe background, alternatives considered, potential impact

**Example**:
```
Previous event statistics only counted total events.
Adding per-type counters helps identify hot paths and
debug event storms.

Trade-off: increases event_system_cb_t by 4 bytes.
```

### 3.5.6 `<footer>` Footer (Optional)

Reference Issues or indicate breaking changes:
```
Closes: #123
Closes: #456
References: #789

BREAKING CHANGE: event_subscribe() now requires non-NULL callback.
```

### 3.5.7 Common Commit Examples

| Scenario | Example Commit Message |
|----------|------------------------|
| Add new feature | `feat(module): add example_module_gpio module` |
| Bug fix | `fix(event): fix event dispatcher ISR race condition` |
| Performance optimization | `perf(sys_memory): optimize first-fit algorithm` |
| Documentation update | `docs(docs): update Thread_IPC integration guide` |
| Refactor code | `refactor(event): simplify event_system_init flow` |
| Formatting | `style(src): clang-format event_system.c` |
| Test case | `test(event): add event queue overflow test` |
| CI configuration | `ci(github): add nucleo_l4r5zi build task` |

### 3.5.8 Git Configuration Suggestions

**Set default editor and auto trim**:
```bash
git config commit.verbose true
git config grep.lineNumber true
```

**Commit Template (optional)**:
Create `.gitmessage` in project root:
```
<type>(<scope>): <subject>

<motivation>
Why this change is needed.

<implementation>
How the problem is solved.

<footer>
Closes: #
```

Load template:
```bash
git config commit.template .gitmessage
```

---

## 4. Continuous Integration (CI)

GitHub Actions is in **`.github/workflows/ci.yml`**; GitLab is in **`.gitlab-ci.yml`**. For steps to enable and troubleshoot CI on these platforms, see the **[CI Platform Configuration Guide.md](../50-testing-ci/52-ci-platform-setup.md)**. Pipelines typically include:

- **ShellCheck**: Static analysis for **`scripts/*.sh`**.  
- **pre-commit**: Aligned with **`.pre-commit-config.yaml`** (includes **clang-format**, YAML, trailing whitespace, etc.); locally install with `pip install pre-commit && pre-commit install` to align with CI.  
- **Build / Test**: **`native_sim`** main project and **`tests/`**, plus multi-ARM board matrix smoke tests.

Local Zephyr and CI image versions should be aligned; see the **[Zephyr Version and CI Guide.md](../70-release-productization/72-zephyr-version-ci.md)**. Ensure CI is **green** before merging PRs or explain failure reasons (e.g., known limitations triggered by documentation-only changes).

---

## 5. Change Scope and Review-friendly Practices

- **Each PR focuses on a single topic**; avoid opportunistic refactors of unrelated files so review and rollback stay manageable.
- **Do not commit** local paths, secrets, or large binaries (for secrets-related topics, see the **[Security and Key Management Guide.md](../70-release-productization/75-security-key-management.md)**).
- For changes to **`prj.conf` / `app.overlay`**, if they affect RAM or board-level behavior, please explain **target board** and **verification method** in the PR description.

---

## 6. License

Contributed content is by default subject to the repository root **[LICENSE](../../../LICENSE)** (GPL-3.0). If introducing third-party code, **source and license compatibility** must be explained in the PR.
