> Language: [中文](../../zh-CN/50-测试与CI/52-CI平台配置保姆级手册.md) | **English**

# CI Platform Setup Guide (GitHub / GitLab)

This document is for readers **first** configuring **Continuous Integration (CI)** for this repository (or projects based on this template) on hosted platforms: explaining **what the CI pipeline does, how to enable it step-by-step on GitHub Actions and GitLab CI, view results, modify versions and board types**, and **common issues**.

**Related Documents**: [Zephyr Version and CI Guide.md](../70-release-productization/72-zephyr-version-ci.md) · [Unit Testing and CI Guide.md](51-unit-testing-ci.md) · [Security and Key Management Guide.md](../70-release-productization/75-security-key-management.md)

---

## 1. Understanding: What is Running in This Repository's CI?

Regardless of using GitHub or GitLab, the core approach of this template is consistent:

| Stage | What It Does | Dependencies |
|-------|-------------|--------------|
| **Code Quality** | `shellcheck` checks `scripts/*.sh`; `pre-commit run --all-files` (including **clang-format**, YAML, trailing whitespace, etc., see `**.pre-commit-config.yaml**`) | Only needs Ubuntu + tools, **does not need** Zephyr source |
| **Build / Test** | Executes `**west init -l .**` → `**west update**` → `**west zephyr-export**` inside container `**gcr.io/zephyr-project/zephyr-build:v<version>**`, then `**west build**` (Zephyr's `revision` in root `**west.yml**` should align) | Needs access to GitHub (to pull Zephyr) |
| **Documentation** | **Doxygen** generates `**docs/api/`** (if `**Doxyfile**` exists) | Only needs **doxygen** / **graphviz** |
| **Release (GitHub only, current config)** | Collects build artifacts and creates **Release** when `**v***` tag is pushed | See below "GitHub · Tag Release" |

**Single Source of Version Number**: `**ZEPHYR_VERSION`** in the workflow (currently `4.3.0`) should match container tag `**zephyr-build:v4.3.0**` and `**revision: v4.3.0**` in root `**west.yml`**; change all three together when upgrading. See **[Zephyr Version and CI Guide](../70-release-productization/72-zephyr-version-ci.md)** for details.

---

## 2. GitHub Actions (Pre-configured in This Repository)

Config file path: `**.github/workflows/ci.yml**`. Follow the "zero to running" steps below.

### 2.1 Put Your Code on GitHub

1. Create a new empty repository on GitHub (can be set to Private).
2. On local, enter project root (if Git not initialized yet):
   ```bash
    git init
    git remote add origin https://github.com/<your username>/<repo name>.git
    git add .
    git commit -m "Initial commit"
    git branch -M main
    git push -u origin main
   ```
3. If you already have a remote, just `**git remote set-url origin ...**` then `**git push**`.

### 2.2 Confirm Actions are Enabled

1. Open the repository page on GitHub.
2. Go to **Settings → Actions → General**.
3. **Actions permissions** select **Allow all actions and reusable workflows** (or follow company policy to "only allow listed" and allow `**actions/*`**).
4. If organization policy disables Actions, ask organization admin to enable.

### 2.3 Trigger Your First Pipeline

1. Push any commit to `**main**` or `**develop**`, or create a **Pull Request** to `**main**` - this triggers `**.github/workflows/ci.yml`** based on `on:` configuration.
2. Open repository tab **Actions**, you should see workflow run records for **"Zephyr Build CI"**.
3. Click into a run, expand the **Job** list (for example **Code Quality**, **Build (native_sim)**, and **Unit Tests (native_sim)**), then inspect the failed step logs.

### 2.4 Common Failures and Self-Troubleshooting

| Symptom | Possible Cause | Resolution |
|---------|---------------|------------|
| **west update timeout / cannot clone Zephyr** | Runner restricted from accessing GitHub | Change network, use company proxy, or configure HTTP(S) proxy on self-hosted Runner; do not hardcode tokens in repo |
| **pre-commit / clang-format failed** | Not run formatting locally | Run `**pre-commit run --all-files**` locally or fix corresponding files per logs then push again |
| **ShellCheck failed** | `scripts/*.sh` doesn't comply with check rules | Fix per log line numbers, or add `**# shellcheck disable=...**` where truly necessary (use sparingly) |
| **west build for some ARM board failed** | Application config incompatible with that board | Modify `**matrix.board**` in `**build-arm**` in `**.github/workflows/ci.yml**`, or modify `**prj.conf**` / overlay; troubleshoot by aligning with local `**west build -b <board name>**` |
| **Forked repo Actions don't run** | Need to enable manually by default | After forking, go to **Actions** page, click **I understand my workflows, go ahead and enable them** |

### 2.5 Modifying CI Behavior (Items You'll Commonly Change)

1. **Change Zephyr Version**
   - Open `**.github/workflows/ci.yml**`, modify `**env: ZEPHYR_VERSION**` at top.
   - Synchronously modify `**revision:**` in root `**west.yml**` (recommend `**vX.Y.Z**` tag).
   - Synchronously update **[Zephyr Version and CI Guide.md](../70-release-productization/72-zephyr-version-ci.md)** and **README** notes if they have hardcoded versions.
2. **Change ARM Smoke Board Types**
   - Add/remove board names in `**build-arm**`'s `**matrix.board`**; names must be Zephyr-supported `**BOARD**` values.
3. **Change Trigger Branches**
   - Modify `**on: push: branches`** / `**pull_request: branches**`, e.g., keep only `**main**`.
4. **Scheduled Jobs**
   - `**schedule: cron**` runs weekly on Sunday; delete `**schedule**` section if not needed.

### 2.6 Tag Release and Permissions (Optional)

The `**release**` job at the end of the workflow executes when `**push**` matches `refs/tags/v***` (see `**if: startsWith(github.ref, 'refs/tags/v')**`).

1. Tag locally and push:
   ```bash
    git tag v1.0.1
    git push origin v1.0.1
   ```
2. **GITHUB_TOKEN** is automatically injected by Actions, generally **no need** to configure in Secrets for uploading Release assets.
3. If using `**softprops/action-gh-release**` and enterprise policy restricts third-party Actions, allow that Action in **Settings → Actions** or switch to official steps.

### 2.7 When Are Secrets Needed?

This template **does not require** custom Secrets by default for building. Add in **Settings → Secrets and variables → Actions** only in these situations, for example:

- Pulling Zephyr from a **private** Git mirror (need to construct URL with Secret in script, **do not** print to logs);
- Publishing to non-GitHub artifact repositories (needs separate job and Token).

See **[Security and Key Management Guide.md](../70-release-productization/75-security-key-management.md)** for key management principles.

---

## 3. GitLab CI (Using `.gitlab-ci.yml` in Repository)

GitHub and GitLab are **mutually exclusive choices** but can also **both be used**: maintain separate pipelines on both sides, **please sync** `**ZEPHYR_VERSION**`, board matrix, and `**west.yml**` when modifying.

### 3.1 Preparation: Push Code to GitLab

1. Create project on GitLab (**gitlab.com** or self-hosted instance), can import GitHub repo or create empty project then add remote:
   ```bash
    git remote add gitlab https://gitlab.com/<group>/<project>.git
    git push -u gitlab main
   ```
2. Ensure `**.gitlab-ci.yml**` exists in repo root (this repo provides an example, aligned with `**.github/workflows/ci.yml`** capabilities).

### 3.2 Enable CI/CD

1. Go to project **Settings → General → Visibility, project features, permissions**.
2. Confirm **CI/CD** is enabled (usually enabled by default).
3. **Settings → CI/CD → General pipelines**: can set `**Timeout**`, `**Test coverage parsing**` as needed; beginners can keep defaults.

### 3.3 Runner: Shared Runner or Self-Hosted?

- **GitLab.com Shared Runners**: Can use directly within free quota; **no need** to set up your own machine, pipelines appear in **CI/CD → Pipelines** after pushing code.
- **Self-hosted Runner**: Used when inside company intranet, needing access to private dependencies, or pulling `**gcr.io**` is slow; register Runner in **Settings → CI/CD → Runners** per documentation; admin needs to ensure Runner can access **GitHub** (`**west update**` pulls Zephyr) and **Google Container Registry** (pulls `**zephyr-build**` image).

If pulling `**gcr.io/zephyr-project/zephyr-build**` fails:

- Check Runner network and firewall;
- Or sync the image to an enterprise mirror, then change `**image:**` in `**.gitlab-ci.yml**` to intranet mirror address.

### 3.4 Viewing Pipeline Results

1. Menu **CI/CD → Pipelines**: list shows each pipeline with status (passed / failed).
2. Click into one → **Jobs**: corresponds to GitHub's Job (e.g., **code_quality**, **build_native**, **build_tests**, **build_arm**, etc.).
3. When failed, click into Job to see **logs**; after fixing, **git push** triggers new pipeline.

### 3.5 Modifying GitLab CI Behavior

1. Edit root `**.gitlab-ci.yml**`.
2. When modifying `**ZEPHYR_VERSION**` at top, must sync `**image: gcr.io/zephyr-project/zephyr-build:v$ZEPHYR_VERSION**` (image tag is `**v` + version number**) with `**west.yml**`.
3. `**build_arm**` uses `**parallel: matrix**` with `**BOARD**` list; changing board names is same as GitHub's **matrix.board**.
4. If a Job is temporarily not needed, comment out entire Job section, or add condition `**rules:if: '$CI_PIPELINE_SOURCE == "merge_request_event"'`** (see [GitLab CI documentation](https://docs.gitlab.com/ee/ci/yaml/)).

### 3.6 GitLab Variables and Secrets (Optional)

Path: **Settings → CI/CD → Variables**.

- `**ZEPHYR_VERSION**` is already defined in `**variables:**` in `**.gitlab-ci.yml`**; can also be overridden in UI (note to align with `**west.yml`**).
- If private dependency tokens are needed in the future, check **Masked**, **Protected**, and **do not** `**echo**` variable values in scripts.

---

## 4. Dual Platform Comparison (At-a-Glance Alignment for Maintenance)

| Item | GitHub | GitLab |
|------|--------|--------|
| Config File | `**.github/workflows/ci.yml**` | `**.gitlab-ci.yml**` |
| Zephyr Container | `**gcr.io/zephyr-project/zephyr-build:v${{ env.ZEPHYR_VERSION }}**` | `**gcr.io/zephyr-project/zephyr-build:v$ZEPHYR_VERSION**` |
| Application Zephyr Source | `**west.yml**` + `**west update**` | Same |
| View Results | **Actions** page | **CI/CD → Pipelines** |
| Quality Gate | **Code Quality** job | **code_quality** job |

---

## 5. Other Platforms (Gitee, Self-Hosted Jenkins, etc.)

- **Same approach**: Prepare environment with Zephyr SDK toolchain (or directly use `**zephyr-build**` image), install **west**, execute same CI scripts `**west init -l .**` → `**west update**` → `**west zephyr-export**` → `**west build ...**` in pipeline.
- **Gitee**: If providing GitHub-like Actions or Jenkins, translate `**steps**` in `**.github/workflows/ci.yml**` to corresponding syntax; note whether Runner can access **GitHub** and **gcr.io**.
- **Do not** commit `**zephyr_config.env**` or private keys to repo; inject path-type config via **variables** on Runner.

---

## 6. Recommended Self-Check (After CI Changes)

1. Local **`west build -b native_sim .`** and **`west build -b native_sim tests/`** + **`run`** pass on Linux/WSL.
2. `**revision**` in `**west.yml**` matches `**ZEPHYR_VERSION**` / image.
3. After push, platform shows **all pipelines passed** (or known allowed failures are noted in MR).

---

*If you add/remove jobs or board matrix entries, update the descriptions in the **[Unit Testing and CI Guide.md](51-unit-testing-ci.md)** and **[Zephyr Version and CI Guide.md](../70-release-productization/72-zephyr-version-ci.md)**.*
