> Language: [中文](../../zh-CN/70-发布与产品化/73-发布检查清单.md) | **English**

# Release Checklist

This checklist guides the complete process of project version releases, ensuring release quality and consistency.

---

## Table of Contents

- [Pre-Release Preparation](#pre-release-preparation)
- [Version Release Detailed Steps](#version-release-detailed-steps)
- [GitHub Release Creation Guide](#github-release-creation-guide)
- [GitLab Release Creation Guide](#gitlab-release-creation-guide)
- [Post-Release Verification](#post-release-verification)
- [Appendices](#appendices)

---

## Pre-Release Preparation

### Code Quality

- [ ] **Run all unit tests and pass**

  ```bash
  # Linux/WSL: run the CI-aligned native_sim tests (Zephyr 4.3.0)
  west build -b native_sim tests/ --build-dir build_tests
  west build -t run --build-dir build_tests
  ```

- [ ] **Run clang-format on code**

  ```bash
  # Linux/macOS
  find src -name "*.c" -o -name "*.h" | xargs clang-format -i

  # Windows PowerShell
  Get-ChildItem -Recurse -Include *.c,*.h src/ | ForEach-Object { clang-format -i $_.FullName }

  # Windows CMD
  for /r src %f in (*.c *.h) do @clang-format -i "%f"

  # Or use pre-commit (recommended, cross-platform consistent)
  pre-commit run --all-files
  ```

- [ ] **Run clang-tidy static analysis (optional)**

  > **Note**: This step is **optional**; skip if clang toolchain is not installed, or use CI for checking.

  **Method 1: Use CI for checking (recommended, no local installation needed)**

  After pushing code, GitHub Actions will automatically run clang-tidy checks; view results on the Actions page.

  **Method 2: Run locally (requires LLVM/Clang installed)**

  ```bash
  # Windows PowerShell (requires LLVM installed first):
  Get-ChildItem -Recurse -Include *.c src/ | ForEach-Object { clang-tidy $_.FullName -- -Isrc }

  # Windows CMD:
  for /r src %f in (*.c) do @clang-tidy "%f" -- -Isrc

  # Linux/macOS:
  clang-tidy src/**/*.c -- -Isrc -I$ZEPHYR_BASE/include
  ```

  **Method 3: Use pre-commit (automatic checking)**

  ```bash
  # Install pre-commit hooks
  pre-commit install

  # Automatically runs clang-tidy on commit
  git commit -m "your message"
  ```

  > **Note**: `clang-tidy` requires compile database (`compile_commands.json`); run a build first.

- [ ] **Confirm no compilation warnings**

  ```bash
  # Clean and rebuild, check for warnings
  west build -t pristine -b <your_board> .
  # Check output for no warnings
  ```

- [ ] **Prepare release notes**

  This repository does not currently maintain a standalone changelog. Prepare the version notes in the GitHub/GitLab Release, for example:

  ```markdown
  ## [1.1.0] - 2026-04-01

  ### Added
  - Added board migration guide documentation
  - Added device tree migration detailed steps

  ### Changed
  - Optimized memory configuration process
  - Updated documentation index

  ### Fixed
  - Fixed stack overflow issue on small RAM boards
  - Corrected overlay documentation

  ### Removed
  - Removed redundant boards/app.overlay
  ```

### Documentation

- [ ] **Update README.md (if necessary)**
  - Update features list
  - Update version number (`bump_version.py` will auto-update)
  - Update quick start examples (if changed)

- [ ] **Generate API documentation**

  ```bash
  # Windows (PowerShell)
  .\scripts\generate_docs.ps1

  # Linux/macOS
  ./scripts/generate_docs.sh

  # Or run Doxygen manually
  doxygen Doxyfile
  ```

- [ ] **Update related documentation**
  - `docs/en/10-environment-build/11-environment-setup-guide.md` (if config changed)
  - `docs/en/10-environment-build/13-board-migration-guide.md` (if new board support added)
  - `docs/en/60-debug-troubleshoot/62-common-issues-troubleshooting.md` (if new issues solved)

- [ ] **Verify documentation matches code**
  - Check API docs match actual code
  - Check example code is runnable
  - Check screenshots match current UI

### Version Management

- [ ] **Confirm current version number**

  ```bash
  # View current version
  python scripts/bump_version.py
  # Or check APP_VERSION file
  cat APP_VERSION
  ```

- [ ] **Determine new version number** (follow Semantic Versioning spec)
  - **Major**: Incompatible API changes
  - **Minor**: Backward-compatible feature additions
  - **Patch**: Backward-compatible bug fixes

- [ ] **Update version number**

  ```bash
  # Update version (auto-syncs to APP_VERSION, Doxyfile, README.md)
  python scripts/bump_version.py 1.1.0

  # Verify update
  git diff
  ```

- [ ] **Commit version changes**

  ```bash
  git add APP_VERSION Doxyfile README.md
  git commit -m "chore: Update version to 1.1.0"
  ```

### CI/CD

- [ ] **Confirm GitHub Actions build succeeds**
  - Visit `https://github.com/<owner>/<repo>/actions`
  - Check latest workflow run status is green

- [ ] **Confirm all platform builds pass**
  - [ ] `nucleo_l4r5zi` build passes
  - [ ] `nucleo_h743zi` build passes
  - [ ] `native_sim` build passes

- [ ] **Confirm unit tests pass**
  - [ ] `native_sim` test task passes
  - [ ] Test coverage meets expectations

- [ ] **Confirm documentation generates successfully**
  - [ ] Doxygen documentation generates without errors
  - [ ] API docs accessible

---

## Version Release Detailed Steps

### Step 1: Prepare Workspace

```bash
# 1. Switch to main branch
git checkout main

# 2. Pull latest code
git pull origin main

# 3. Ensure workspace is clean
git status
# Should show "nothing to commit, working tree clean"
```

### Step 2: Run Local Tests

```bash
# 1. Run unit tests
west build -b native_sim tests/ --build-dir build_tests
west build -t run --build-dir build_tests

# 2. Build target board (verify no compilation errors)
west build -t pristine -b nucleo_l4r5zi .

# 3. Check memory usage
arm-none-eabi-size build/zephyr/zephyr.elf
```

### Step 3: Update Version Number

```bash
# 1. View current version
python scripts/bump_version.py
# Output: 1.0.0

# 2. Update to new version
python scripts/bump_version.py 1.1.0
# Output: Version set to 1.1.0 (was 1.0.0)

# 3. Verify changed files
git status
# Should show APP_VERSION, Doxyfile, README.md modified
```

### Step 4: Prepare Release Notes

Prepare the new version record in the Release draft:

```markdown
## [1.1.0] - 2026-04-01

### Added
- Added board migration guide documentation
- Added device tree migration detailed steps
- Added MCU memory parameters quick reference

### Changed
- Optimized memory configuration process
- Updated documentation index
- Improved overlay documentation

### Fixed
- Fixed stack overflow issue on small RAM boards
- Corrected overlay auto-load documentation
- Fixed documentation link errors

### Deprecated
- None

### Removed
- Removed redundant boards/app.overlay
```

### Step 5: Commit All Changes

```bash
# 1. Add all changed files
git add APP_VERSION Doxyfile README.md docs/

# 2. Commit changes
git commit -m "chore: Prepare for v1.1.0 release

- Updated version to 1.1.0
- Prepared release notes
- Updated documentation"

# 3. Push to remote
git push origin main
```

### Step 6: Wait for CI Verification

- [ ] Visit GitHub Actions page
- [ ] Confirm latest commit workflow runs successfully
- [ ] Confirm all build tasks pass
- [ ] Confirm documentation generates successfully

**If CI fails**:
1. Check failure logs
2. Fix issues
3. Re-commit and push
4. Wait for CI to run again

### Step 7: Create Git Tag

```bash
# 1. Create annotated tag
git tag -a v1.1.0 -m "Release v1.1.0

Major updates:
- Added board migration guide
- Optimized memory configuration process
- Fixed small RAM board issues

See the Release notes for details"

# 2. Verify tag
git tag -l
# Should display v1.1.0

# 3. View tag details
git show v1.1.0
```

### Step 8: Push Tags

```bash
# Push tag to remote
git push origin v1.1.0

# Or push all tags
git push origin --tags
```

### Step 9: Trigger CI Build (Optional)

After pushing tags, GitHub Actions will automatically run tagged builds. If not auto-triggered:

```bash
# Manual trigger (if workflow_dispatch is configured)
# Manually run on GitHub Actions page
```

---

## GitHub Release Creation Guide

### Method 1: Using GitHub Web Interface (Recommended)

1. **Visit Releases page**
   ```
   https://github.com/<owner>/<repo>/releases
   ```

2. **Click "Draft a new release"**

3. **Fill in release info**
   - **Tag version**: Select `v1.1.0` (just pushed tag)
   - **Release title**: `v1.1.0`
   - **Description**: Paste CHANGELOG content or auto-generate

4. **Add build artifacts** (optional)
   - Drag and drop `.elf`, `.bin` files
   - Upload API docs ZIP

5. **Set release type**
   - [x] Set as the latest release (if official release)
   - [ ] This is a pre-release (if pre-release)
   - [ ] Create a discussion for this release

6. **Click "Publish release"**

### Method 2: Using GitHub CLI

```bash
# 1. Install GitHub CLI (if not installed)
# Windows: winget install GitHub.cli
# macOS: brew install gh
# Linux: sudo apt install gh

# 2. Login to GitHub
gh auth login

# 3. Create Release (auto-generate notes)
gh release create v1.1.0 --generate-notes

# 4. Or manually specify title and notes
gh release create v1.1.0 \
  --title "v1.1.0" \
  --notes "Paste the reviewed release notes here"

# 5. Upload build artifacts
gh release upload v1.1.0 \
  build/zephyr/zephyr.elf \
  build/zephyr/zephyr.bin \
  docs/api/html.zip
```

### Method 3: Using GitHub API

```bash
# Create Release using curl
curl -X POST \
  -H "Authorization: token $GITHUB_TOKEN" \
  -H "Accept: application/vnd.github.v3+json" \
  https://api.github.com/repos/<owner>/<repo>/releases \
  -d '{
    "tag_name": "v1.1.0",
    "name": "v1.1.0",
    "body": "Release notes...",
    "draft": false,
    "prerelease": false
  }'
```

### Uploading Build Artifacts

```bash
# 1. Build Release version
west build -t pristine -b nucleo_l4r5zi -DCONFIG_DEBUG=n .

# 2. Copy build artifacts
mkdir -p release_artifacts
cp build/zephyr/zephyr.elf release_artifacts/
cp build/zephyr/zephyr.bin release_artifacts/

# 3. Package API docs
cd docs/api/html
zip -r ../html.zip .
cd ../../..
cp docs/api/html.zip release_artifacts/

# 4. Upload to Release (using gh CLI)
gh release upload v1.1.0 release_artifacts/*
```

---

## GitLab Release Creation Guide

### Method 1: Using GitLab Web Interface

1. **Visit Releases page**
   ```
   https://gitlab.com/<owner>/<repo>/-/releases
   ```

2. **Click "New release"**

3. **Fill in release info**
   - **Tag name**: `v1.1.0`
   - **Release name**: `v1.1.0`
   - **Description**: Paste the reviewed release notes

4. **Click "Create release"**

### Method 2: Using GitLab CLI

```bash
# Create Release using glab
glab release create v1.1.0 --notes "Paste the reviewed release notes here"
```

### Method 3: Auto-create in `.gitlab-ci.yml`

```yaml
release:
  stage: deploy
  image: registry.gitlab.com/gitlab-org/release-cli:latest
  rules:
    - if: $CI_COMMIT_TAG  # Run only when pushing tags
  release:
    tag_name: $CI_COMMIT_TAG
    name: "Release $CI_COMMIT_TAG"
    description: "See the release notes for details"
    assets:
      links:
        - name: "Firmware ELF"
          url: "${CI_PROJECT_URL}/-/jobs/${CI_JOB_ID}/artifacts/build/zephyr/zephyr.elf"
        - name: "Firmware BIN"
          url: "${CI_PROJECT_URL}/-/jobs/${CI_JOB_ID}/artifacts/build/zephyr/zephyr.bin"
```

---

## Post-Release Verification

### Verify Release

- [ ] **Download and test Release**

  ```bash
  # 1. Download Release artifacts
  gh release download v1.1.0

  # 2. Flash to development board
  west flash

  # 3. Verify version info
  west console
  # Input: version
  # Should display new version number
  ```

- [ ] **Verify documentation accessible**
  - [ ] API documentation links valid
  - [ ] User manual downloadable
  - [ ] README displays correctly

- [ ] **Verify example code runs**
  - [ ] Quick start example runs
  - [ ] Example code matches documentation

### Notifications and Announcements

- [ ] **Publish release announcement**
  - Post update announcement on project home page
  - Post in relevant forums/communities

- [ ] **Notify team members**
  - Send email notification
  - Announce in team chat groups

- [ ] **Update project home page**
  - Update project README Release badges
  - Update download links

### Follow-up Work

- [ ] **Create new development branch** (if needed)

  ```bash
  # Create maintenance branch
  git checkout -b release/1.1.x
  git push origin release/1.1.x
  ```

- [ ] **Update development plan**
  - Plan next version features
  - Create Issues and Milestones

- [ ] **Collect user feedback**
  - Monitor Issues and discussions
  - Collect user usage data

---

## Appendices

### A. Semantic Versioning Specification

```
Version number format: X.Y.Z

- X (Major): Incompatible API changes
- Y (Minor): Backward-compatible feature additions
- Z (Patch): Backward-compatible bug fixes

Examples:
1.0.0 -> 1.0.1  # Bug fix
1.0.0 -> 1.1.0  # New feature (backward compatible)
1.0.0 -> 2.0.0  # Breaking changes
```

### B. Quick Release Commands Summary

```bash
# ========== Preparation Phase ==========
# 1. Switch to main and update
git checkout main
git pull origin main

# 2. Run tests
west build -b native_sim tests/ --build-dir build_tests
west build -t run --build-dir build_tests

# 3. Format code (choose command for your system):

# Linux/macOS:
find src -name "*.c" -o -name "*.h" | xargs clang-format -i

# Windows PowerShell:
Get-ChildItem -Recurse -Include *.c,*.h src/ | ForEach-Object { clang-format -i $_.FullName }

# Windows CMD:
for /r src %f in (*.c *.h) do @clang-format -i "%f"

# Or use pre-commit (recommended, cross-platform):
pre-commit run --all-files

# 4. Run static analysis (optional, skip if clang not installed):

# Recommended: Use CI checking (no local installation needed)
# After pushing code, GitHub Actions will auto-run

# Optional: Run locally (requires LLVM/Clang installed first)
# Windows PowerShell:
Get-ChildItem -Recurse -Include *.c src/ | ForEach-Object { clang-tidy $_.FullName -- -Isrc }

# Windows CMD:
for /r src %f in (*.c) do @clang-tidy "%f" -- -Isrc

# Linux/macOS:
clang-tidy src/**/*.c -- -Isrc -I$ZEPHYR_BASE/include

# ========== Version Update ==========
# 5. Update version
python scripts/bump_version.py 1.1.0

# 6. Prepare release notes

# 7. Commit changes
git add APP_VERSION Doxyfile README.md docs/
git commit -m "chore: Prepare for v1.1.0 release"
git push origin main

# ========== Wait for CI Verification ==========
# Visit https://github.com/<owner>/<repo>/actions to confirm build succeeds

# ========== Create Tag ==========
# 8. Create Git tag
git tag -a v1.1.0 -m "Release v1.1.0"

# 9. Push tag
git push origin v1.1.0

# ========== Create Release ==========
# 10. Create Release using GitHub CLI
gh release create v1.1.0 --generate-notes

# 11. Upload build artifacts
west build -t pristine -b nucleo_l4r5zi .
gh release upload v1.1.0 build/zephyr/zephyr.elf build/zephyr/zephyr.bin

# ========== Verify ==========
# 12. Download and test Release
gh release download v1.1.0
```

### C. Version Release Checklist (Print Version)

```
Release Checklist v1.1.0
=======================

Pre-Release Preparation:
[ ] Unit tests pass
[ ] Code formatting complete
[ ] Static analysis passes
[ ] No compilation warnings
[ ] Release notes prepared
[ ] Documentation updated
[ ] API docs generated
[ ] CI builds pass

Version Release:
[ ] Version number updated
[ ] Changes committed and pushed
[ ] Git tag created
[ ] Tag pushed
[ ] GitHub Release created
[ ] Build artifacts uploaded

Post-Release Verification:
[ ] Release download test passes
[ ] Documentation links valid
[ ] Example code runs
[ ] Team notified
[ ] Announcement posted

Date: ____________
Released by: __________
```

### D. FAQ

**Q1: Build info doesn't change after version update?**

A: Clean CMake cache and rebuild:
```bash
west build -t pristine
```

**Q2: Git Dirty flag always 1?**

A: Ensure all changes are committed, or clean generated files:
```bash
git clean -fdx
```

**Q3: CI build fails?**

A: Check failure logs, fix issues, and re-commit push.

**Q4: Want to modify Release after creation?**

A: You can edit the Release page to modify notes and attachments, or delete and recreate.

**Q5: clang-format/clang-tidy command not found?**

A: These tools require LLVM/Clang toolchain installation:

**Installation**:

```bash
# Windows: Install LLVM (includes clang-format and clang-tidy)
winget install LLVM.LLVM
# Or download from https://releases.llvm.org/download.html
# After installation, may need to restart or add to PATH: C:\Program Files\LLVM\bin

# macOS: Use Homebrew
brew install clang-format clang-tools-extra

# Linux (Ubuntu/Debian):
sudo apt install clang-format clang-tidy

# Linux (Fedora/RHEL):
sudo dnf install clang-tools-extra
```

**If you don't want to install locally**, you can:
1. **Use CI checking** (recommended): GitHub Actions auto-runs clang-tidy after code push
2. **Skip this step**: clang-tidy is optional, doesn't affect release

**run-clang-tidy installation** (if needed):
```bash
pip install run-clang-tidy
```

**Q6: find/xargs commands error on Windows?**

A: Windows CMD doesn't support Unix find command, use:
- PowerShell commands (see above)
- Or CMD for loop (see above)
- Or install WSL/Git Bash to use Unix commands

---

**Version**: 1.1.0
**Last Updated**: 2026-04-01
**Maintainers**: Project Maintenance Team
