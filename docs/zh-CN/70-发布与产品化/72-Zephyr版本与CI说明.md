> 语言: **中文** | [English](../../en/70-release-productization/72-zephyr-version-ci.md)

# Zephyr 版本与 CI 说明

为减少「本地能编、CI 失败」或反之的差异，建议将 **本地环境** 与 **持续集成** 对齐到同一主线。

**相关文档**：[文档索引.md](../00-入门/02-文档索引.md) · [版本管理.md](71-版本管理.md)（`APP_VERSION` 与固件展示）。

## CI 使用的 Zephyr 版本

以下两处应使用**同一** Zephyr 主线（镜像标签为 **`v` + 版本号**，如 **`v4.4.0`**）：

| 平台 | 配置文件 | 变量 / 镜像 |
|------|----------|-------------|
| **GitHub Actions** | `.github/workflows/ci.yml` | `env.ZEPHYR_VERSION`；`image: gcr.io/zephyr-project/zephyr-build:v${{ env.ZEPHYR_VERSION }}` |
| **GitLab CI** | `.gitlab-ci.yml` | `variables.ZEPHYR_VERSION`；`image: gcr.io/zephyr-project/zephyr-build:v$ZEPHYR_VERSION` |

合并或发版前，若升级 CI 中的 Zephyr 版本，请同步：

1. 本地 `ZEPHYR_BASE` 指向的 Zephyr 仓库检出 **兼容 tag 或分支**。
2. [Zephyr SDK](https://github.com/zephyrproject-rtos/sdk-ng/releases) 与 [Zephyr 文档](https://docs.zephyrproject.org/) 中该版本要求的工具链版本。
3. 本仓库 **`west.yml`** 中 **`revision:`**（建议 **tag**，如 **`v4.4.0`**）。
4. 若使用 GitLab，同步 **`.gitlab-ci.yml`** 中的 **`ZEPHYR_VERSION`**（与 GitHub 一致）。
5. 本仓库 `README.md` 中关于前提条件 / CI 的说明（若有硬编码版本号）。

**在托管平台上启用 / 维护 CI 的步骤**（含排查失败）：**[CI平台配置保姆级手册.md](../50-测试与CI/52-CI平台配置保姆级手册.md)**。

## West 工作区（可选）

若使用根目录 `west.yml` 管理 Zephyr：

- 将 `projects.zephyr.revision` 固定为 **tag**（如 `v4.4.0`）而非浮动的 `main`，便于复现构建。
- 首次 `west update` 后执行 `west zephyr-export`，与 `zephyr_config.env` 中的 `ZEPHYR_BASE` 一致。

## 应用版本（固件版本号）

固件展示用版本号与 **仓库 `APP_VERSION` 文件**、CMake、`Doxyfile`、README 的同步方式见 **[版本管理.md](71-版本管理.md)** 与脚本 `scripts/bump_version.py`。
