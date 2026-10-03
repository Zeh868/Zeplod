> Language: [中文](../../zh-CN/80-贡献与维护/83-AI协作开发指南.md) | **English**

# AI-Assisted Development Guide

This guide explains how humans and AI agents collaborate on Zeplod. The executable agent rules live in
[`AGENTS.md`](../../../AGENTS.md); this page explains the workflow and governance behind those rules.

## Principles and roles

- AI assists implementation and review; it is not the source of truth or the final approver.
- Resolve conflicts in this order: current code/Kconfig/CMake/CI, canonical documentation, then README/examples/history.
- Use capability roles—human owner, orchestrator, implementation agent, and review agent—instead of binding policy to a model name.
- Model selection belongs to caller configuration and must not weaken verification requirements.

## Required task packet

Every delegated task should state its objective, allowed and forbidden scope, constraints, relevant context, executable acceptance
checks, and the required return format. Agents must stop and escalate when progress requires new authority, credentials, unavailable
hardware, or a product decision.

## Verification evidence

Report the exact commands, exit codes, concise pass/fail evidence, and every check that was not run. Never turn “expected to pass”
into “verified.” Re-run a failed check after fixing it. Finish with `git diff --check` and `git status --short`.

## Zephyr risk areas

- Call only explicitly ISR-safe APIs from an ISR; do not block, take a mutex, or perform uncertain allocation.
- Check the state-machine, lock-order, and lifecycle contracts before changing concurrent services.
- Read defaults from Kconfig and CI versions/platforms from the workflow; do not copy constants from older documents.
- Treat hardware behavior as unverified until supported by an appropriate build, test, or device result.

## Security, privacy, and licensing

Do not expose secrets, certificates, private keys, customer data, local configuration, or internal credentials. Follow the
organization's data policy before sending proprietary code to a hosted model. Review generated code for license compatibility and
third-party attribution. Security, cryptography, and key-management changes require human review.

## Return format

- **Summary**: result and verdict
- **Changed files**: grouped by purpose
- **Verification**: commands, exit codes, and evidence
- **Open decisions**: only items that still require a human decision

When Zephyr, CI platforms, test matrices, or public APIs change, review this guide, `AGENTS.md`, task navigation, the code/document
map, and the relevant canonical guide together.
