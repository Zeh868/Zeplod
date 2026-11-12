#!/usr/bin/env python3
"""在 Zephyr QEMU 板上运行 tests/ ztest，并按串口结果给出退出码。

native_sim / native_posix 是主机进程，不走本脚本。当前只接受已核对的
qemu_riscv32：Zephyr virt 机器，仿真器为 qemu-system-riscv32。

v0.29.4 镜像的 Dockerfile.base 不 apt 安装 qemu-system-*。二进制来自
Zephyr SDK hosttools（setup.sh -h），CMake 通过 CMAKE_PREFIX_PATH 查找。

QEMU 在打印 PROJECT EXECUTION SUCCESSFUL 后不会自己退出（与 native_sim 的
posix_exit 不同）。本脚本用 QEMU_PIPE 采集串口，看到结束横幅或超时后结束进程。

退出码：0 成功；1 断言失败、panic 或缺少成功横幅；124 超时；2 用法或找不到仿真器。
不覆盖物理 PHY、板级外设和真实时序。
"""

from __future__ import annotations

import argparse
import os
import select
import signal
import subprocess
import sys
import time
from collections.abc import Mapping
from pathlib import Path

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_USAGE = 2
EXIT_TIMEOUT = 124

SUCCESS_BANNER = "PROJECT EXECUTION SUCCESSFUL"
FAILED_BANNER = "PROJECT EXECUTION FAILED"
ASSERT_MARKER = "Assertion failed at"
PANIC_MARKERS = (
    "ZEPHYR FATAL ERROR",
    "fatal error was unexpected",
    "assert failed was unexpected",
)
HOST_BOARDS = frozenset({"native_sim", "native_posix"})
SUPPORTED_BOARD = "qemu_riscv32"
QEMU_BINARY = "qemu-system-riscv32"
GRACE_SECONDS = 2.0
DEFAULT_CONF = "prj.conf;prj_test_extensions.conf"
DEFAULT_TIMEOUT = 600.0


class BoardError(Exception):
    """请求的板型不能当作本仓库的 QEMU 运行时目标。"""


class SerialVerdict:
    """串口文本对应的进程退出码和原因。"""

    def __init__(self, code: int, reason: str) -> None:
        self.code = code
        self.reason = reason


def strip_ansi(text: str) -> str:
    """去掉串口里的 ANSI 颜色序列和回车。"""
    out: list[str] = []
    index = 0
    while index < len(text):
        if text[index] == "\x1b" and index + 1 < len(text) and text[index + 1] == "[":
            index += 2
            while index < len(text) and text[index] not in "ABCDEFGHJKSTfmnsulh":
                index += 1
            index += 1
            continue
        char = text[index]
        if char != "\r":
            out.append(char)
        index += 1
    return "".join(out)


def classify_serial(text: str, timed_out: bool = False) -> SerialVerdict:
    """根据完整串口判断 ztest 结果。

    断言、panic 或 PROJECT EXECUTION FAILED 优先于成功横幅。
    仅有成功横幅时返回 0，即使 QEMU 进程随后被脚本关掉。
    """
    cleaned = strip_ansi(text)
    if (
        ASSERT_MARKER in cleaned
        or FAILED_BANNER in cleaned
        or any(marker in cleaned for marker in PANIC_MARKERS)
    ):
        return SerialVerdict(EXIT_FAIL, "assertion, panic, or PROJECT EXECUTION FAILED")
    if SUCCESS_BANNER in cleaned:
        return SerialVerdict(EXIT_OK, "PROJECT EXECUTION SUCCESSFUL")
    if timed_out:
        return SerialVerdict(EXIT_TIMEOUT, "timeout before PROJECT EXECUTION banner")
    return SerialVerdict(EXIT_FAIL, "missing PROJECT EXECUTION SUCCESSFUL")


def validate_board(board: str) -> str:
    """返回该板需要的 qemu-system 二进制名。"""
    name = board.strip()
    if name in HOST_BOARDS:
        raise BoardError(
            f"{name} is a host process target, not QEMU. "
            "Keep native_sim on the host job and use qemu_riscv32 for emulated CPU runtime."
        )
    if name != SUPPORTED_BOARD:
        raise BoardError(
            f"unsupported QEMU board {name!r}. "
            "Runtime coverage is qemu_riscv32 only (Zephyr virt, qemu-system-riscv32). "
            "Other QEMU boards, including qemu_cortex_m3, are not a verified runtime target."
        )
    return QEMU_BINARY


def _executable(path: Path) -> bool:
    return path.is_file() and os.access(path, os.X_OK)


def _sdk_roots(env: Mapping[str, str]) -> list[Path]:
    sdk = env.get("ZEPHYR_SDK_INSTALL_DIR", "").strip()
    if sdk:
        return [Path(sdk)]
    roots: list[Path] = []
    for parent in (Path("/opt/toolchains"), Path("/opt")):
        if parent.is_dir():
            roots.extend(sorted(parent.glob("zephyr-sdk-*")))
    return roots


def sdk_install_dir_from_qemu(qemu: Path) -> Path | None:
    """从 SDK hosttools 里的 qemu 路径回推安装根目录。"""
    for parent in qemu.parents:
        if parent.name.startswith("zephyr-sdk-"):
            return parent
    return None


def find_qemu_binary(name: str, env: Mapping[str, str]) -> Path | None:
    """在 QEMU_BIN_PATH、PATH 和 SDK hosttools 中查找仿真器。"""
    candidates: list[Path] = []
    configured = env.get("QEMU_BIN_PATH", "").strip()
    if configured:
        location = Path(configured)
        if location.is_file():
            candidates.append(location)
        else:
            candidates.append(location / name)
    for entry in env.get("PATH", "").split(os.pathsep):
        if entry:
            candidates.append(Path(entry) / name)
    for root in _sdk_roots(env):
        candidates.extend(root.glob(f"hosttools/sysroots/*/usr/bin/{name}"))
    seen: set[Path] = set()
    for candidate in candidates:
        try:
            resolved = candidate.resolve()
        except OSError:
            continue
        if resolved in seen:
            continue
        seen.add(resolved)
        if _executable(resolved) and resolved.name == name:
            return resolved
    return None


def prepare_fifos(base: Path) -> None:
    """创建 QEMU chardev pipe 使用的 base.in 与 base.out。"""
    base.parent.mkdir(parents=True, exist_ok=True)
    if base.exists():
        base.unlink()
    for suffix in (".in", ".out"):
        pipe = Path(str(base) + suffix)
        if pipe.exists():
            pipe.unlink()
        os.mkfifo(pipe, 0o600)


def kill_process_group(proc: subprocess.Popen[object]) -> None:
    """结束 west/QEMU 进程组。重复调用是安全的。"""
    if proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError):
        try:
            proc.terminate()
        except (ProcessLookupError, OSError):
            return
    try:
        proc.wait(timeout=3)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        try:
            proc.kill()
        except (ProcessLookupError, OSError):
            return
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        pass


def _line_is_terminal(line: str) -> bool:
    return SUCCESS_BANNER in line or FAILED_BANNER in line or "ZEPHYR FATAL ERROR" in line


def capture_serial(
    base: Path,
    proc: subprocess.Popen[object],
    timeout: float,
    log_path: Path,
) -> tuple[str, bool]:
    """从 fifo 读取串口，直到结束横幅后的短暂窗口或超时。

    返回 (文本, 是否在看到结束横幅前超时)。调用方负责在返回后进程已被结束。
    """
    out_path = Path(str(base) + ".out")
    in_path = Path(str(base) + ".in")
    log_path.parent.mkdir(parents=True, exist_ok=True)
    # 先占住输入端，QEMU 打开 .in 读端时不会一直阻塞。
    in_fd = os.open(in_path, os.O_RDWR)
    out_fd = os.open(out_path, os.O_RDONLY | os.O_NONBLOCK)
    lines: list[str] = []
    pending = bytearray()
    saw_terminal = False
    timed_out = False
    deadline = time.monotonic() + timeout
    poller = select.poll()
    poller.register(out_fd, select.POLLIN | select.POLLHUP)

    def consume(chunk: bytes) -> None:
        nonlocal deadline, saw_terminal
        pending.extend(chunk)
        while b"\n" in pending:
            raw, _, rest = pending.partition(b"\n")
            pending[:] = rest
            line = strip_ansi(raw.decode("utf-8", "replace"))
            lines.append(line)
            log.write(line + "\n")
            log.flush()
            print(line, flush=True)
            if _line_is_terminal(line) and not saw_terminal:
                saw_terminal = True
                grace_end = time.monotonic() + GRACE_SECONDS
                if grace_end < deadline:
                    deadline = grace_end

    try:
        with log_path.open("a", encoding="utf-8") as log:
            while True:
                now = time.monotonic()
                if now >= deadline:
                    timed_out = not saw_terminal
                    break
                events = poller.poll(max(0, int((deadline - now) * 1000)))
                if not events:
                    if proc.poll() is not None:
                        break
                    continue
                revents = events[0][1]
                if revents & select.POLLIN:
                    try:
                        chunk = os.read(out_fd, 4096)
                    except BlockingIOError:
                        chunk = b""
                    if chunk:
                        consume(chunk)
                        continue
                if revents & select.POLLHUP:
                    break
            if pending:
                line = strip_ansi(pending.decode("utf-8", "replace"))
                if line:
                    lines.append(line)
                    log.write(line + "\n")
                    log.flush()
                    print(line, flush=True)
    finally:
        os.close(out_fd)
        os.close(in_fd)
        kill_process_group(proc)
    text = "\n".join(lines)
    if text:
        text += "\n"
    return text, timed_out


def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def _run_and_tee(cmd: list[str], cwd: Path, env: dict[str, str], log) -> int:
    proc = subprocess.Popen(
        cmd,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        sys.stdout.write(line)
        log.write(line)
        log.flush()
    return proc.wait()


def _kill_qemu_pidfile(build_dir: Path) -> None:
    pid_path = build_dir / "qemu.pid"
    if not pid_path.is_file():
        return
    try:
        pid = int(pid_path.read_text(encoding="utf-8").strip())
    except ValueError:
        return
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.kill(pid, sig)
        except (ProcessLookupError, PermissionError):
            return
        time.sleep(0.2)


def main(argv: list[str] | None = None) -> int:
    """构建并运行 qemu_riscv32 ztest。"""
    parser = argparse.ArgumentParser(
        description=(
            "Run tests/ ztest on qemu_riscv32. This is QEMU, not native_sim. "
            "Physical PHY, board peripherals, and wall-clock timing are not covered. "
            "Exit 0 only for PROJECT EXECUTION SUCCESSFUL without assertion or panic; "
            "124 on timeout."
        )
    )
    parser.add_argument(
        "--board",
        default=os.environ.get("ZEPHYR_QEMU_TEST_BOARD", SUPPORTED_BOARD),
    )
    parser.add_argument(
        "--conf",
        default=os.environ.get("ZEPHYR_QEMU_TEST_CONF", DEFAULT_CONF),
        help="CONF_FILE passed to tests/ (default: the project ztest extension overlay)",
    )
    parser.add_argument(
        "--build-dir",
        default=os.environ.get("ZEPHYR_QEMU_TEST_BUILD_DIR", "build_tests_qemu"),
    )
    parser.add_argument(
        "--log",
        default=os.environ.get("ZEPHYR_QEMU_TEST_LOG", "qemu-ztest.log"),
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=float(os.environ.get("ZEPHYR_QEMU_TEST_TIMEOUT", DEFAULT_TIMEOUT)),
        help="seconds to wait for a ztest banner before killing QEMU",
    )
    parser.add_argument("--no-pristine", action="store_true")
    args = parser.parse_args(argv)

    try:
        binary_name = validate_board(args.board)
    except BoardError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_USAGE

    qemu = find_qemu_binary(binary_name, os.environ)
    if qemu is None:
        print(
            "error: qemu-system-riscv32 not found in QEMU_BIN_PATH, PATH, or "
            "ZEPHYR_SDK_INSTALL_DIR hosttools. The zephyr-build:v0.29.4 image does not "
            "apt-install qemu-system-*; it is part of the SDK host tools (setup.sh -h).",
            file=sys.stderr,
        )
        return EXIT_USAGE

    root = _repo_root()
    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = root / build_dir
    log_path = Path(args.log)
    if not log_path.is_absolute():
        log_path = root / log_path
    fifo_base = build_dir / "qemu-fifo"
    env = os.environ.copy()
    env["QEMU_BIN_PATH"] = str(qemu.parent)
    env["QEMU_PIPE"] = str(fifo_base)
    if not env.get("ZEPHYR_SDK_INSTALL_DIR", "").strip():
        sdk_root = sdk_install_dir_from_qemu(qemu)
        if sdk_root is not None:
            env["ZEPHYR_SDK_INSTALL_DIR"] = str(sdk_root)

    build_cmd = [
        "west",
        "build",
        "-b",
        args.board,
        str(root / "tests"),
        "--build-dir",
        str(build_dir),
    ]
    if not args.no_pristine:
        build_cmd.extend(["-p", "always"])
    build_cmd.extend(
        [
            "--",
            f"-DCONF_FILE={args.conf}",
            f"-DQEMU_PIPE={fifo_base}",
        ]
    )
    print(f"QEMU board: {args.board} (not native_sim)")
    print(f"QEMU binary: {qemu}")
    print(f"ZEPHYR_SDK_INSTALL_DIR: {env.get('ZEPHYR_SDK_INSTALL_DIR', '')}")
    print(f"CONF_FILE: {args.conf}")
    print(f"serial timeout: {args.timeout:.0f}s")
    print("coverage: tests/ ztest logic on qemu_riscv32; physical PHY/peripherals/timing not covered")

    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as log:
        log.write("# " + " ".join(build_cmd) + "\n")
        try:
            build_rc = _run_and_tee(build_cmd, root, env, log)
        except FileNotFoundError:
            log.write("\n# west not found\n")
            print("error: west not found on PATH", file=sys.stderr)
            return EXIT_USAGE
        if build_rc != 0:
            log.write(f"\n# west build failed: {build_rc}\n")
            print(f"error: west build failed ({build_rc}). See {log_path}", file=sys.stderr)
            return build_rc

    prepare_fifos(fifo_base)
    run_cmd = ["west", "build", "-t", "run", "--build-dir", str(build_dir)]
    runner_log = build_dir / "qemu-runner.stdout"
    runner_log.parent.mkdir(parents=True, exist_ok=True)
    with runner_log.open("w", encoding="utf-8") as runner_out:
        proc = subprocess.Popen(
            run_cmd,
            cwd=root,
            env=env,
            stdout=runner_out,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            start_new_session=True,
        )
        try:
            text, timed_out = capture_serial(fifo_base, proc, args.timeout, log_path)
        finally:
            _kill_qemu_pidfile(build_dir)
    runner_text = ""
    if runner_log.is_file():
        runner_text = runner_log.read_text(encoding="utf-8", errors="replace")
    with log_path.open("a", encoding="utf-8") as log:
        if runner_text:
            log.write("\n# west build -t run stdout\n")
            log.write(runner_text)
            if not runner_text.endswith("\n"):
                log.write("\n")
    verdict = classify_serial(text + "\n" + runner_text, timed_out=timed_out)
    print(verdict.reason)
    if verdict.code != 0:
        print(f"QEMU ztest failed ({verdict.code}). Serial log: {log_path}", file=sys.stderr)
    return verdict.code


if __name__ == "__main__":
    sys.exit(main())
