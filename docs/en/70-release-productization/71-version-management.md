> Language: [中文](../../zh-CN/70-发布与产品化/71-版本管理.md) | **English**

# Software Version Management Guide

This project's software version management system provides complete version tracking, build information, and Git integration.

## Version Information Components

### 1. Version Number

Uses Semantic Versioning: `Major.Minor.Patch`

- **Major**: Incompatible API changes
- **Minor**: Backward-compatible feature additions
- **Patch**: Backward-compatible bug fixes

**Single Source of Truth**: The `APP_VERSION` file in the repository root (single line `X.Y.Z`). `CMakeLists.txt` reads this file during configuration and defines `PROJECT_VERSION_*`; to update the external version, edit `APP_VERSION` or run:

**DO NOT** use the root directory filename `VERSION`: Zephyr's `find_package(Zephyr)` parses the kernel version field by this filename, causing configuration errors like `VERSION_MAJOR must be present`.

```bash
python scripts/bump_version.py 1.0.1
```

The above script syncs `PROJECT_NUMBER` in `Doxyfile` and the "**Version**" line in `README.md`; run `git diff` before committing to verify.

### 2. Git Information

- **Commit Hash**: Short hash of the current commit
- **Branch**: Branch name at build time
- **Tag**: Git tag (if any)
- **Dirty Flag**: Whether there are uncommitted changes

### 3. Build Information

- **Build Date**: `YYYY-MM-DD`
- **Build Time**: `HH:MM:SS`
- **Target Board**: Build target

## Log Output Example

Startup log output:

```
[00:00:00.100] ========================================
[00:00:00.101]   Application Version Information
[00:00:00.102] ========================================
[00:00:00.103]   Version:     1.0.0
[00:00:00.104]   Version Code: 0x010000
[00:00:00.105]   Git Commit:  a1b2c3d
[00:00:00.106]   Git Branch:  main
[00:00:00.107]   Git Tag:     v1.0.0
[00:00:00.108]   Build Time:  2026-03-28 14:30:00
[00:00:00.109]   Build Target: nucleo_f429zi
[00:00:00.110]   Build Type:  Release
[00:00:00.111]   Compiler:    GCC 12.2.1
[00:00:00.112] ========================================
```

## Usage

### Getting Version Information

```c
#include <zeplod/app_version.h>

// Get version string
char version[VERSION_STRING_MAX_LEN];
app_version_get_string(version, sizeof(version));
// Output: "1.0.0"

// Get full version info
char info[VERSION_INFO_STRING_MAX_LEN];
app_version_get_info_string(info, sizeof(info));
// Output: "v1.0.0 (a1b2c3d) [Release] 2026-03-28 14:30:00 - nucleo_f429zi"

// Get version code
uint32_t code = app_version_get_code();  // 0x010000
uint8_t major = app_version_get_major();  // 1
uint8_t minor = app_version_get_minor();  // 0
uint8_t patch = app_version_get_patch();  // 0

// Get Git info
const char *commit = app_version_get_git_commit();  // "a1b2c3d"
const char *branch = app_version_get_git_branch();  // "main"

// Get build timestamp
const char *timestamp = app_version_get_build_timestamp();  // "2026-03-28 14:30:00"

// Print version info to log
app_version_print();

// Version check
if (app_version_check(1, 0, 0)) {
    // Version matches
}
```

### Shell Commands

```bash
# View version information
version

# App status (includes version)
app status
```

### Version Comparison

```c
#include <zeplod/app_version.h>

// Check if version is at least 1.0.0
if (VERSION_AT_LEAST(1, 0, 0)) {
    // Feature available
}

// Check if version is exactly 1.0.0
if (VERSION_IS(1, 0, 0)) {
    // Exact version match
}

// Check if version is at most 2.0.0
if (VERSION_AT_MOST(2, 0, 0)) {
    // Within range
}
```

## Modifying Version Numbers

The repo-root `APP_VERSION` file is the single source of truth for the application version.
Use the version script to update it and the related documentation references; do not edit
the derived variables in `CMakeLists.txt` directly:

```bash
python scripts/bump_version.py 1.1.0
```

At release time, create a Git tag that matches `APP_VERSION`:

```bash
# Create version tag
git tag -a v1.1.0 -m "Release version 1.1.0"

# Push tag
git push origin v1.1.0
```

## Version Encoding

Version encoding is a 24-bit integer:

```
Bits 23-16: Major version (8 bits)
Bits 15-8:  Minor version (8 bits)
Bits 7-0:   Patch version (8 bits)

Example: 1.2.3 = 0x010203 = 66051
```

## Version Macro Injection

Version macros are injected at build time via `target_compile_definitions` (`-D`)
in `CMakeLists.txt`, sourced from the repo-root `APP_VERSION` file:

- `PROJECT_VERSION_MAJOR/MINOR/PATCH`, `PROJECT_VERSION` - application version (from `APP_VERSION`)
- `GIT_COMMIT_HASH`, `GIT_BRANCH`, `GIT_TAG`, `GIT_DIRTY` - Git information
- `BUILD_TIMESTAMP`, `BUILD_DATE`, `BUILD_TIME`, `BUILD_TARGET` - build information

`app_version.h` consumes these macros (with safe `#ifndef` fallbacks).

## CI/CD Integration

GitHub Actions automatically records version information on each build:

```yaml
- name: Get version info
  run: |
    echo "Version: ${{ github.ref_name }}"
    echo "Commit: ${{ github.sha }}"
```

## Best Practices

1. **Update version number before each release** - Follow Semantic Versioning spec
2. **Use Git tags** - Easy tracking of release versions
3. **Prepare release notes** - This repository does not currently maintain a standalone changelog; record the changes in the Release notes
4. **Check version compatibility** - Verify versions during module communication
5. **Preserve version info** - Embed version string in firmware

## Troubleshooting

### Issue: Version info shows "unknown"

**Cause**: Git not found or not in a Git repository

**Solution**:
```bash
# Ensure you're in a Git repository
git status

# Initialize Git if needed
git init
git add .
git commit -m "Initial commit"
```

### Issue: Build time not updating

**Cause**: CMake cache

**Solution**:
```bash
# Clean and rebuild
rm -rf build/
west build -b <board> .
```

### Issue: Git Dirty flag always 1

**Cause**: Uncommitted changes or generated files

**Solution**:
```bash
# Commit changes
git add .
git commit -m "Update"

# Or clean generated files
git clean -fdx
```

## Related Files

| File | Description |
|------|-------------|
| `APP_VERSION` | Single source of truth for the version number (X.Y.Z) |
| `include/zeplod/app_version.h` | Version API header |
| `src/app/app_version.c` | Version API implementation |
| `CMakeLists.txt` | Reads `APP_VERSION`, injects version macros via `-D` |

## References

- [Semantic Versioning 2.0.0](https://semver.org/)
- [Git Version Control](https://git-scm.com/book/en/v2)
