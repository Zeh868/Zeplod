> Language: [中文](../../zh-CN/70-发布与产品化/75-安全与密钥管理说明.md) | **English**

# Security and Key Management Guide

This article explains from a **repository and process** perspective: what content **must not** be committed to Git, how keys should be managed in OTA/signing scenarios, and precautions for local configurations like **`zephyr_config.env`**. **Does not replace** Zephyr/MCUboot official security whitepaper; mass production security requires threat modeling and independent audit.

**Related**: **[OTA and Storage Expansion Guide.md](74-ota-storage-guide.md)** · **[Contributing and Code Standards.md](../80-contributing/81-contributing-code-style.md)**

---

## 1. Content Prohibited from Version Control

| Type | Description |
|------|-------------|
| **Private keys and certificates** | OTA signing private keys, TLS client certificate private keys, code signing keys, etc. |
| **Passwords and Tokens** | Wi-Fi passwords, cloud API keys, GitHub PAT, cloud service credentials |
| **Sensitive info in absolute local paths** | If path exposes internal network structure, decide whether to anonymize per team policy |
| **Logs/dumps containing customer data** | Debug artifacts should not be committed as documentation |

If **example configuration** is genuinely needed, use obvious placeholders (e.g., `YOUR_KEY_ID`, fake hostname), with comments indicating "example, do not use in production".

---

## 2. `zephyr_config.env` and `.gitignore`

- **`zephyr_config.env`** is used to fill in local **`ZEPHYR_BASE`**, **`ZEPHYR_SDK_INSTALL_DIR`**, etc., usually copied from **`zephyr_config.env.template`**.
- This file usually **should not contain** keys; if team accidentally writes keys into env, immediately **rotate keys** and clean from history (if necessary, use `git filter-repo` and similar tools, requiring repository admin).
- Confirm **`.gitignore`** includes `zephyr_config.env` (or equivalent rule) to prevent pushing to remote.

---

## 3. OTA / Image Signing (MCUboot, etc.)

Key points when integrating **MCUboot**, **image signing**:

- **Private keys** are only stored in **secure media** (HSM, dedicated signing machine, controlled CI key vault), **do not** appear in application repository or developer laptop plaintext paths.
- Signing in CI: Use **GitHub Actions / GitLab** provided **Secrets** to inject key material, **do not** hardcode in workflow.
- **Public keys/hashes** can be distributed with firmware or partition descriptions for device-side verification; algorithm and key length per product security level.

Process and Kconfig symbols per current Zephyr documentation **[Device Management / MCUboot](https://docs.zephyrproject.org/latest/services/device_mgmt/mcuboot.html)**; this repository's **[OTA and Storage Expansion Guide.md](74-ota-storage-guide.md)** is only architecture-level guidance.

---

## 4. Debug and Release Images

- **Development** firmware can have more logs and debug interfaces; **production images** should close debug backdoors, tighten log levels, and enable **read protection**, **secure boot** as needed (depending on SoC; see chip vendor documentation).
- Before release, verify **[Release Checklist.md](73-release-checklist.md)**, confirming no test keys or debug symbol policies were accidentally included.

---

## 5. When a Leak is Discovered

If a key has been accidentally committed to Git:

1. **Immediately invalidate** that key and issue a new one (treat as compromised).
2. **Delete** sensitive files from repository and clean history (requires repository admin permissions).
3. Notify team to rotate all systems using that key (CI, device-side public key update strategy, etc.).

---

*This article updates with repository practices; consult security and legal for compliance and certification matters.*
