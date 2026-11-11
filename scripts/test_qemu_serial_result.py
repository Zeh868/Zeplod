#!/usr/bin/env python3
"""qemu_ztest_run 的判定与串口采集测试。

这些检查先于 CI 接线存在：断言失败、panic、超时必须非 0，
PROJECT EXECUTION SUCCESSFUL 且无故障才是 0。native_sim 不是 QEMU。
"""

from __future__ import annotations

import os
import stat
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import qemu_ztest_run as qz  # noqa: E402


class ClassifyTests(unittest.TestCase):
    def test_success_is_zero(self) -> None:
        verdict = qz.classify_serial(
            "*** Booting Zephyr OS\nRunning TESTSUITE event_system\n"
            "PROJECT EXECUTION SUCCESSFUL\n",
            timed_out=False,
        )
        self.assertEqual(verdict.code, 0)
        self.assertIn("SUCCESSFUL", verdict.reason)

    def test_success_after_qemu_kept_running_is_still_pass(self) -> None:
        """west run 在横幅后不会让 QEMU 自己退出；采集层会杀掉进程。"""
        verdict = qz.classify_serial(
            "PROJECT EXECUTION SUCCESSFUL\n",
            timed_out=False,
        )
        self.assertEqual(verdict.code, 0)

    def test_assertion_failure_is_nonzero(self) -> None:
        verdict = qz.classify_serial(
            "Assertion failed at tests/test_event_system.c:10\n"
            "PROJECT EXECUTION FAILED\n",
            timed_out=False,
        )
        self.assertNotEqual(verdict.code, 0)

    def test_assertion_without_final_banner_is_nonzero(self) -> None:
        verdict = qz.classify_serial(
            "Assertion failed at tests/test_event_system.c:10\n",
            timed_out=True,
        )
        self.assertNotEqual(verdict.code, 0)
        self.assertNotEqual(verdict.code, qz.EXIT_TIMEOUT)

    def test_assertion_plus_success_banner_is_nonzero(self) -> None:
        verdict = qz.classify_serial(
            "Assertion failed at tests/test_event_system.c:10\n"
            "PROJECT EXECUTION SUCCESSFUL\n",
            timed_out=False,
        )
        self.assertNotEqual(verdict.code, 0)

    def test_panic_after_success_is_nonzero(self) -> None:
        verdict = qz.classify_serial(
            "PROJECT EXECUTION SUCCESSFUL\n"
            ">>> ZEPHYR FATAL ERROR 0: CPU exception on CPU 0\n",
            timed_out=False,
        )
        self.assertNotEqual(verdict.code, 0)

    def test_ansi_wrapped_failure_is_nonzero(self) -> None:
        verdict = qz.classify_serial(
            "\x1b[31mPROJECT EXECUTION FAILED\x1b[0m\n",
            timed_out=False,
        )
        self.assertNotEqual(verdict.code, 0)

    def test_timeout_without_banner_is_timeout_code(self) -> None:
        verdict = qz.classify_serial("*** Booting Zephyr OS\n", timed_out=True)
        self.assertEqual(verdict.code, qz.EXIT_TIMEOUT)

    def test_missing_banner_without_timeout_is_nonzero(self) -> None:
        verdict = qz.classify_serial("*** Booting Zephyr OS\n", timed_out=False)
        self.assertNotEqual(verdict.code, 0)
        self.assertNotEqual(verdict.code, qz.EXIT_TIMEOUT)


class BoardTests(unittest.TestCase):
    def test_native_sim_is_rejected(self) -> None:
        with self.assertRaises(qz.BoardError) as ctx:
            qz.validate_board("native_sim")
        self.assertIn("native_sim", str(ctx.exception))
        self.assertIn("QEMU", str(ctx.exception))

    def test_native_posix_is_rejected(self) -> None:
        with self.assertRaises(qz.BoardError):
            qz.validate_board("native_posix")

    def test_unverified_qemu_board_is_rejected(self) -> None:
        with self.assertRaises(qz.BoardError):
            qz.validate_board("qemu_cortex_m3")

    def test_riscv32_binary_name(self) -> None:
        self.assertEqual(qz.validate_board("qemu_riscv32"), "qemu-system-riscv32")


class MainTests(unittest.TestCase):
    def test_main_rejects_native_sim_before_build(self) -> None:
        code = qz.main(["--board", "native_sim"])
        self.assertEqual(code, qz.EXIT_USAGE)


class FindQemuTests(unittest.TestCase):
    def test_qemu_bin_path_wins(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            binary = directory / "qemu-system-riscv32"
            binary.write_text("#!/bin/sh\n", encoding="utf-8")
            binary.chmod(binary.stat().st_mode | stat.S_IEXEC)
            found = qz.find_qemu_binary(
                "qemu-system-riscv32",
                {"QEMU_BIN_PATH": str(directory), "PATH": ""},
            )
            self.assertEqual(found, binary.resolve())

    def test_sdk_hosttools_layout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            sdk = Path(tmp)
            bindir = sdk / "hosttools" / "sysroots" / "x86_64-pokysdk-linux" / "usr" / "bin"
            bindir.mkdir(parents=True)
            binary = bindir / "qemu-system-riscv32"
            binary.write_text("#!/bin/sh\n", encoding="utf-8")
            binary.chmod(binary.stat().st_mode | stat.S_IEXEC)
            found = qz.find_qemu_binary(
                "qemu-system-riscv32",
                {"PATH": "", "ZEPHYR_SDK_INSTALL_DIR": str(sdk)},
            )
            self.assertEqual(found, binary.resolve())

    def test_missing_binary_returns_none(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            found = qz.find_qemu_binary(
                "qemu-system-riscv32",
                {"PATH": tmp, "QEMU_BIN_PATH": "", "ZEPHYR_SDK_INSTALL_DIR": tmp},
            )
            self.assertIsNone(found)


class CaptureTests(unittest.TestCase):
    def _writer(self, fifo_base: Path, script: str) -> subprocess.Popen[bytes]:
        return subprocess.Popen(
            [sys.executable, "-c", script, str(fifo_base)],
            start_new_session=True,
        )

    def test_success_stops_without_waiting_for_full_timeout(self) -> None:
        script = textwrap.dedent(
            """
            import os, sys, time
            base = sys.argv[1]
            out = os.open(base + ".out", os.O_WRONLY)
            os.write(out, b"*** Booting Zephyr OS\\n")
            os.write(out, b"PROJECT EXECUTION SUCCESSFUL\\n")
            os.write(out, b"still alive\\n")
            time.sleep(30)
            """
        )
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp) / "qemu-fifo"
            qz.prepare_fifos(base)
            proc = self._writer(base, script)
            log_path = Path(tmp) / "serial.log"
            started = time.monotonic()
            try:
                text, timed_out = qz.capture_serial(base, proc, timeout=8, log_path=log_path)
            finally:
                qz.kill_process_group(proc)
            elapsed = time.monotonic() - started
            verdict = qz.classify_serial(text, timed_out=timed_out)
            self.assertEqual(verdict.code, 0)
            self.assertFalse(timed_out)
            self.assertLess(elapsed, 6)
            self.assertIn("PROJECT EXECUTION SUCCESSFUL", log_path.read_text(encoding="utf-8"))
            self.assertIsNotNone(proc.poll())

    def test_timeout_kills_hung_writer(self) -> None:
        script = textwrap.dedent(
            """
            import os, sys, time
            base = sys.argv[1]
            out = os.open(base + ".out", os.O_WRONLY)
            os.write(out, b"*** Booting Zephyr OS\\n")
            time.sleep(30)
            """
        )
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp) / "qemu-fifo"
            qz.prepare_fifos(base)
            proc = self._writer(base, script)
            log_path = Path(tmp) / "serial.log"
            started = time.monotonic()
            try:
                text, timed_out = qz.capture_serial(base, proc, timeout=1, log_path=log_path)
            finally:
                qz.kill_process_group(proc)
            elapsed = time.monotonic() - started
            verdict = qz.classify_serial(text, timed_out=timed_out)
            self.assertTrue(timed_out)
            self.assertEqual(verdict.code, qz.EXIT_TIMEOUT)
            self.assertLess(elapsed, 5)
            self.assertIsNotNone(proc.poll())


if __name__ == "__main__":
    unittest.main()
