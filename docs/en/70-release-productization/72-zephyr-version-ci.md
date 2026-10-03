> Language: [中文](../../zh-CN/70-发布与产品化/72-Zephyr版本与CI说明.md) | **English**

# Zephyr Version and CI Guide

To reduce discrepancies between "works locally, fails in CI" or vice versa, it is recommended to align your **local environment** and **continuous integration** to the same mainline.

**Related Documents**: [Documentation Index.md](../00-getting-started/02-doc-index.md) · [Version Management.md](71-version-management.md) (`APP_VERSION` and firmware display).

## Zephyr Version Used in CI

Both locations should use the **same** Zephyr mainline (mirror tag is **`v` + version number**, like **`v4.3.0`**):

| Platform | Config File | Variable / Image |
|----------|-------------|------------------|
| **GitHub Actions** | `.github/workflows/ci.yml` | `env.ZEPHYR_VERSION`; `image: gcr.io/zephyr-project/zephyr-build:v${{ env.ZEPHYR_VERSION }}` |
| **GitLab CI** | `.gitlab-ci.yml` | `variables.ZEPHYR_VERSION`; `image: gcr.io/zephyr-project/zephyr-build:v$ZEPHYR_VERSION` |

Before merging or releasing, if upgrading Zephyr version in CI, also sync:

1. Local `ZEPHYR_BASE` pointing to Zephyr repo checkout **compatible tag or branch**.
2. Toolchain version required by [Zephyr SDK](https://github.com/zephyrproject-rtos/sdk-ng/releases) and [Zephyr Documentation](https://docs.zephyrproject.org/) for that version.
3. **`revision:`** in this repository's **`west.yml`** (recommend **tag**, like **`v4.3.0`**).
4. If using GitLab, sync **`ZEPHYR_VERSION`** in **`.gitlab-ci.yml`** (consistent with GitHub).
5. Any hardcoded version numbers in this repository's `README.md` regarding prerequisites/CI (if applicable).

**Steps to enable/maintain CI on hosted platforms** (including troubleshooting failures): **[CI Platform Configuration Guide.md](../50-testing-ci/52-ci-platform-setup.md)**.

## West Workspace (Optional)

If using root `west.yml` to manage Zephyr:

- Fix `projects.zephyr.revision` to a **tag** (e.g., `v4.3.0`) instead of floating `main`, for reproducible builds.
- After first `west update`, run `west zephyr-export`, consistent with `ZEPHYR_BASE` in `zephyr_config.env`.

## Application Version (Firmware Version Number)

For firmware display version number synchronization with repository `APP_VERSION` file, CMake, `Doxyfile`, and README, see **[Version Management.md](71-version-management.md)** and script `scripts/bump_version.py`.
