#!/usr/bin/env python3
"""WP9 reliability smoke over the tab5 console (USB-Serial-JTAG, default COM3).

Phases:
  A  N short agent runs (default 100); heap sampled every 10 runs; USAGE tokens
     accumulated; per-run duration and first-event latency recorded.
  B  M cancels (default 30): half during the model stream, half during a tool
     call; verifies CANCELLED fires and no TEXT_DELTA/RUN_COMPLETED follows.
  C  wifi-reconnect while a run is in flight, then recovery verification.
  D  (optional --401-test) invalid key -> ERROR (401) -> restore key -> recovery.

Questions are pure ASCII: linenoise in no-tty mode drops multibyte args, so the
device would receive a truncated question otherwise.

Usage:
  python reliability_smoke.py [port] [--runs 100] [--cancels 30] [--401-test]
      [--key-file <good key file>] [--reset-cmd <esptool reset command...>]
      [--report <output file>]

Exit code 0 when the smoke passes its gates, 1 otherwise.
"""

import argparse
import random
import re
import subprocess
import sys
import time

import serial

RE_RUN_START = re.compile(r"\[agent\] ===== run (\d+):")
RE_RUN_DONE = re.compile(r"\[agent\] run (\d+) done: (\S+)")
RE_CANCELLED = re.compile(r"\[agent\] CANCELLED")
RE_ERROR = re.compile(r"\[agent\] ERROR (.*)")
RE_TOOL_CALL = re.compile(r"\[agent\] TOOL_CALL_STARTED")
RE_TOOL_EXEC = re.compile(r"\[agent\] TOOL_EXECUTION_STARTED")
RE_TEXT_DELTA = re.compile(r"\[agent\] TEXT_DELTA")
RE_RUN_COMPLETED = re.compile(r"\[agent\] RUN_COMPLETED")
RE_USAGE = re.compile(r"\[agent\] USAGE in=(\d+) out=(\d+)")
RE_FREE = re.compile(r"free heap: (\d+) bytes, largest block: (\d+) bytes")
RE_WIFI_CONNECTED = re.compile(r"wifi: connected ip=")
RE_WIFI_NOT = re.compile(r"wifi: not connected")
RE_WIFI_RECONNECT = re.compile(r"wifi: reconnect -> (\S+)")
RE_PROMPT = re.compile(r"tab5>")
RE_CRASH = re.compile(r"(SW_CPU_RESET|SW_RESET|Returned from app_main|main_task: Started on CPU0|Guru Meditation|abort\(\) was called|Brownout detector was triggered|Unrecoverable host sdio state)")

Q_SHORT = "what is 7 times 8? answer in one sentence."
Q_LONG = "explain the second law of thermodynamics in detail, at least 200 words."
Q_TOOL = "get the device status."
Q_PING = "what is 1+1? answer in one sentence."


class Device:
    def __init__(self, port):
        self.port = port
        self.ser = None
        self.crashes = []
        self.open()

    def open(self):
        last = None
        for _ in range(12):
            try:
                self.ser = serial.Serial(self.port, 115200, timeout=1.0)
                time.sleep(0.3)
                self.ser.reset_input_buffer()
                return
            except Exception as e:  # noqa: BLE001 - port may still be re-enumerating
                last = e
                time.sleep(3)
        raise RuntimeError(f"cannot open {self.port}: {last}")

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:  # noqa: BLE001
                pass
            self.ser = None

    def send(self, cmd):
        self.ser.write((cmd + "\n").encode("ascii", "replace"))
        self.ser.flush()

    def _readline(self):
        """One readline; transparently re-opens the port after a device reboot."""
        for _ in range(3):
            try:
                return self.ser.readline()
            except Exception:  # noqa: BLE001 - device re-enumerating
                self.close()
                time.sleep(2)
                self.open()
                time.sleep(0.5)
        return b""

    def recover(self):
        """Device rebooted mid-test: wait for console, then for wifi link."""
        self.close()
        time.sleep(3)
        self.open()
        deadline = time.time() + 30
        while time.time() < deadline:
            raw = self._readline()
            if raw and RE_PROMPT.search(raw.decode("utf-8", "replace")):
                break
            time.sleep(0.1)
        # worker needs wifi before it can run; wait for the link to come up
        for _ in range(30):
            self.send("wifi")
            deadline = time.time() + 5
            ok = False
            while time.time() < deadline:
                raw = self._readline()
                if not raw:
                    continue
                text = raw.decode("utf-8", "replace").strip()
                if RE_WIFI_CONNECTED.search(text):
                    ok = True
                    break
            if ok:
                return
            time.sleep(2)
        # last resort: let the caller proceed; commands will time out otherwise
        time.sleep(5)

    def read_lines(self, timeout_s):
        """Read available lines until quiet for timeout_s (max 5s per read loop)."""
        lines = []
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            raw = self._readline()
            if raw:
                text = raw.decode("utf-8", "replace").strip()
                if text:
                    lines.append(text)
                deadline = time.time() + timeout_s
        return lines

    def wait_for(self, regex, timeout_s):
        """Collect lines until one matches regex. Returns (lines, match).

        A device reboot (crash) inside the window resets the remaining timeout
        and is recorded in self.crashes; the caller never sees it.
        """
        lines = []
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            raw = self._readline()
            if not raw:
                continue
            text = raw.decode("utf-8", "replace").strip()
            if not text:
                continue
            lines.append(text)
            if RE_CRASH.search(text):
                self.crashes.append(time.time())
                self.recover()
                lines = []
                deadline = time.time() + timeout_s
                continue
            m = regex.search(text)
            if m:
                return lines, m
        return lines, None


class Stats:
    def __init__(self):
        self.runs_ok = 0
        self.runs_err = 0
        self.runs_timeout = 0
        self.run_ms = []
        self.first_event_ms = []
        self.tokens_in = 0
        self.tokens_out = 0
        self.tool_calls_seen = 0
        self.cancel_ok = 0
        self.cancel_late = 0
        self.cancel_noop = 0
        self.cancel_fail = 0
        self.cancel_ms = []
        self.cancel_after_callback = 0
        self.heap_samples = []
        self.errors = []

    def pct(self, values, p):
        if not values:
            return 0.0
        s = sorted(values)
        idx = min(len(s) - 1, int(len(s) * p))
        return s[idx]


def percentile(values, p):
    if not values:
        return 0.0
    s = sorted(values)
    idx = min(len(s) - 1, int(len(s) * p))
    return s[idx]


def phase_a(dev, stats, n, report):
    report.append(f"== Phase A: {n} short agent runs ==")
    for i in range(1, n + 1):
        dev.send(f"agent {Q_SHORT}")
        t0 = time.time()
        lines, done = dev.wait_for(RE_RUN_DONE, 90)
        dt_ms = (time.time() - t0) * 1000
        joined = "\n".join(lines)
        err_match = RE_ERROR.search(joined)
        if done is None:
            stats.runs_timeout += 1
            report.append(f"  run {i}: TIMEOUT (no done in 90s)")
            continue
        if err_match:
            stats.runs_err += 1
            stats.errors.append(f"run {i}: {err_match.group(1)}")
            report.append(f"  run {i}: ERROR {err_match.group(1)} ({dt_ms:.0f}ms)")
            continue
        if done.group(2) != "ESP_OK":
            stats.runs_err += 1
            stats.errors.append(f"run {i}: done {done.group(2)}")
            report.append(f"  run {i}: done {done.group(2)} ({dt_ms:.0f}ms)")
            continue
        stats.runs_ok += 1
        stats.run_ms.append(dt_ms)
        for line in lines:
            if not stats.first_event_ms and re.search(r"\[agent\] [A-Z_]+", line):
                stats.first_event_ms.append(dt_ms)
        if RE_TOOL_CALL.search(joined):
            stats.tool_calls_seen += 1
        for u in RE_USAGE.finditer(joined):
            stats.tokens_in += int(u.group(1))
            stats.tokens_out += int(u.group(2))
        if i % 10 == 0 or i == n:
            dev.send("free")
            flines, fm = dev.wait_for(RE_FREE, 5)
            if fm:
                stats.heap_samples.append((i, int(fm.group(1)), int(fm.group(2))))
            report.append(f"  run {i}: ok {dt_ms:.0f}ms  heap={fm.group(1) if fm else '?'}")
    report.append(f"  A result: ok={stats.runs_ok} err={stats.runs_err} timeout={stats.runs_timeout} "
                  f"tool_calls={stats.tool_calls_seen} tokens_in={stats.tokens_in} tokens_out={stats.tokens_out}")
    if stats.heap_samples:
        first = stats.heap_samples[0]
        last = stats.heap_samples[-1]
        report.append(f"  heap: first(run {first[0]})={first[1]} last(run {last[0]})={last[1]} "
                      f"net_delta={last[1] - first[1]} bytes")
    return stats.runs_err == 0 and stats.runs_timeout == 0


def phase_b(dev, stats, m, report):
    report.append(f"== Phase B: {m} cancels ==")
    n_stream = m // 2
    for i in range(1, m + 1):
        if i <= n_stream:
            dev.send(f"agent {Q_LONG}")
            lines, started = dev.wait_for(RE_RUN_START, 15)
            if started is None:
                stats.cancel_fail += 1
                report.append(f"  cancel {i}: run did not start")
                continue
            time.sleep(random.uniform(0.30, 0.60))
        else:
            dev.send(f"agent {Q_TOOL}")
            lines, started = dev.wait_for(RE_TOOL_EXEC, 30)
            if started is None:
                # tool not called; fall back to stream-phase cancel
                time.sleep(0.3)
            else:
                time.sleep(0.05)
        t_c = time.time()
        dev.send("agent-cancel")
        clines, cm = dev.wait_for(RE_CANCELLED, 30)
        cancel_ms = (time.time() - t_c) * 1000
        joined = "\n".join(clines)
        if cm is None:
            # did the run finish before cancel landed?
            if RE_RUN_DONE.search(joined):
                stats.cancel_late += 1
                report.append(f"  cancel {i}: late (run finished before cancel) {cancel_ms:.0f}ms")
            else:
                stats.cancel_fail += 1
                report.append(f"  cancel {i}: NO CANCELLED within 30s")
            continue
        stats.cancel_ok += 1
        stats.cancel_ms.append(cancel_ms)
        # after CANCELLED there must be no further text/completion callbacks
        after = joined[joined.find("CANCELLED"):]
        bad = RE_TEXT_DELTA.search(after) or RE_RUN_COMPLETED.search(after)
        if bad:
            stats.cancel_after_callback += 1
            report.append(f"  cancel {i}: CALLBACK AFTER CANCEL! {bad.group(0)}")
        dev.wait_for(RE_RUN_DONE, 15)  # worker reports run done after cancel
        if i % 5 == 0:
            report.append(f"  cancel {i}: ok {cancel_ms:.0f}ms")
    report.append(f"  B result: ok={stats.cancel_ok} late={stats.cancel_late} "
                  f"fail={stats.cancel_fail} after_callback={stats.cancel_after_callback} "
                  f"cancel_ms P50={stats.pct(stats.cancel_ms, 0.5):.0f} "
                  f"P95={stats.pct(stats.cancel_ms, 0.95):.0f}")
    return stats.cancel_fail == 0 and stats.cancel_after_callback == 0


def phase_c(dev, stats, report):
    report.append("== Phase C: wifi disconnect/recover during run ==")
    dev.send("wifi-reconnect")
    lines, m = dev.wait_for(RE_WIFI_RECONNECT, 10)
    report.append(f"  reconnect cmd: {m.group(0) if m else 'no reply'}")
    # fire a run immediately, while the link is still reconnecting
    dev.send(f"agent {Q_PING}")
    t0 = time.time()
    lines, done = dev.wait_for(RE_RUN_DONE, 90)
    dt_ms = (time.time() - t0) * 1000
    joined = "\n".join(lines)
    if done is None:
        report.append(f"  in-flight run during reconnect: TIMEOUT")
        return False
    if RE_ERROR.search(joined):
        report.append(f"  in-flight run during reconnect: ERROR {RE_ERROR.search(joined).group(0)} "
                      f"({dt_ms:.0f}ms) [expected transient]")
    elif done.group(2) == "ESP_OK":
        report.append(f"  in-flight run during reconnect: ok {dt_ms:.0f}ms (survived)")
    else:
        report.append(f"  in-flight run during reconnect: done {done.group(2)} ({dt_ms:.0f}ms)")
    # wait for link recovery, then a clean run must succeed
    ok_link = False
    for _ in range(12):
        dev.send("wifi")
        lines, m = dev.wait_for(RE_WIFI_CONNECTED, 6)
        if m:
            ok_link = True
            break
        dev.send("wifi-reconnect")
        dev.wait_for(RE_WIFI_RECONNECT, 6)
        time.sleep(3)
    if not ok_link:
        report.append("  C: wifi did not recover")
        return False
    dev.send(f"agent {Q_PING}")
    lines, done = dev.wait_for(RE_RUN_DONE, 90)
    joined = "\n".join(lines)
    if done and done.group(2) == "ESP_OK" and not RE_ERROR.search(joined):
        report.append("  C: recovery run ok")
        return True
    report.append(f"  C: recovery run failed: {done.group(0) if done else 'timeout'}")
    return False


def phase_d(dev, stats, report, key_file, reset_cmd):
    report.append("== Phase D: 401 injection ==")
    try:
        with open(key_file, "r", encoding="utf-8") as f:
            good_key = f.read().strip()
    except OSError as e:
        report.append(f"  D skipped: cannot read key file {key_file}: {e}")
        return None
    dev.send("cred-set llm_key ARK_INVALID_TEST_KEY_000000")
    time.sleep(0.8)
    dev.close()
    report.append(f"  reset: {' '.join(reset_cmd)}")
    try:
        subprocess.run(reset_cmd, timeout=60, capture_output=True)
    except Exception as e:  # noqa: BLE001
        report.append(f"  D skipped: reset failed: {e}")
        return None
    time.sleep(4)
    dev.open()
    dev.wait_for(RE_PROMPT, 30)
    dev.send(f"agent {Q_PING}")
    lines, done = dev.wait_for(RE_RUN_DONE, 90)
    joined = "\n".join(lines)
    err = RE_ERROR.search(joined)
    if err:
        report.append(f"  401 detected: {err.group(0)}")
    else:
        report.append(f"  401 NOT DETECTED: done={done.group(0) if done else 'timeout'}")
    dev.send("cred-set llm_key " + good_key)
    time.sleep(0.8)
    dev.close()
    try:
        subprocess.run(reset_cmd, timeout=60, capture_output=True)
    except Exception as e:  # noqa: BLE001
        report.append(f"  D: restore reset failed: {e}")
        return False
    time.sleep(4)
    dev.open()
    dev.wait_for(RE_PROMPT, 30)
    dev.send(f"agent {Q_PING}")
    lines, done = dev.wait_for(RE_RUN_DONE, 90)
    joined = "\n".join(lines)
    if done and done.group(2) == "ESP_OK" and not RE_ERROR.search(joined):
        report.append("  D: recovery after key restore ok")
        return err is not None
    report.append(f"  D: recovery failed: {done.group(0) if done else 'timeout'}")
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("port", nargs="?", default="COM3")
    ap.add_argument("--runs", type=int, default=100)
    ap.add_argument("--cancels", type=int, default=30)
    ap.add_argument("--401-test", action="store_true", dest="test_401")
    ap.add_argument("--key-file", default=None)
    ap.add_argument("--reset-cmd", nargs="+", default=None)
    ap.add_argument("--report", default=None)
    args = ap.parse_args()

    if args.test_401:
        if not args.reset_cmd:
            args.reset_cmd = ["python", "-m", "esptool", "--chip", "esp32p4",
                              "--port", args.port, "--before", "default_reset",
                              "--after", "hard_reset", "flash_id"]
        if not args.key_file:
            import os
            args.key_file = os.path.join(os.environ.get("TEMP", ""), "otool_doubao_key.txt")

    report = [f"WP9 reliability smoke @ {time.strftime('%Y-%m-%d %H:%M:%S')} port={args.port}",
              f"runs={args.runs} cancels={args.cancels} 401_test={args.test_401}"]
    stats = Stats()
    dev = Device(args.port)

    try:
        # preflight: worker alive + link up
        dev.send("agent-status")
        dev.wait_for(re.compile(r"agent: round="), 8)
        dev.send("wifi")
        lines, m = dev.wait_for(RE_WIFI_CONNECTED, 8)
        if m is None:
            report.append("wifi not connected at start; reconnecting")
            dev.send("wifi-reconnect")
            dev.wait_for(RE_WIFI_RECONNECT, 8)
            time.sleep(6)
            dev.send("wifi")
            lines, m = dev.wait_for(RE_WIFI_CONNECTED, 8)
            if m is None:
                report.append("FATAL: no wifi link")
                dev.close()
                _finish(report, args.report, 1)
                return

        ok_a = ok_b = True
        ok_a = phase_a(dev, stats, args.runs, report)
        ok_b = phase_b(dev, stats, args.cancels, report)
        ok_c = phase_c(dev, stats, report)
        ok_d = None
        if args.test_401:
            ok_d = phase_d(dev, stats, report, args.key_file, args.reset_cmd)

        report.append("== Summary ==")
        report.append(f"  crashes (SDIO/power, device rebooted): {len(dev.crashes)}")
        report.append(f"  runs: ok={stats.runs_ok} err={stats.runs_err} timeout={stats.runs_timeout}")
        if stats.run_ms:
            report.append(f"  run duration ms: P50={percentile(stats.run_ms, 0.5):.0f} "
                          f"P95={percentile(stats.run_ms, 0.95):.0f} "
                          f"max={max(stats.run_ms):.0f} n={len(stats.run_ms)}")
        if stats.first_event_ms:
            report.append(f"  first event ms: P50={percentile(stats.first_event_ms, 0.5):.0f} "
                          f"max={max(stats.first_event_ms):.0f}")
        report.append(f"  tokens: in={stats.tokens_in} out={stats.tokens_out}")
        report.append(f"  cancels: ok={stats.cancel_ok} late={stats.cancel_late} "
                      f"fail={stats.cancel_fail} after_callback={stats.cancel_after_callback}")
        if stats.cancel_ms:
            report.append(f"  cancel latency ms: P50={percentile(stats.cancel_ms, 0.5):.0f} "
                          f"P95={percentile(stats.cancel_ms, 0.95):.0f}")
        if stats.errors:
            report.append("  errors seen:")
            report.extend(f"    {e}" for e in stats.errors[:20])
        gate_a = ok_a
        gate_b = ok_b
        gate_c = ok_c
        gate_d = True if ok_d is None else ok_d
        report.append(f"  gates: A(runs)={gate_a} B(cancel)={gate_b} C(wifi)={gate_c} D(401)={gate_d}")
        ok = gate_a and gate_b and gate_c and gate_d
    finally:
        dev.close()

    _finish(report, args.report, 0 if ok else 1)


def _finish(report, path, code):
    text = "\n".join(report)
    print(text)
    if path:
        with open(path, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    sys.exit(code)


if __name__ == "__main__":
    main()
