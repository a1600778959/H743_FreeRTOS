"""现有 Make 进度工具的会话计时、主机预算与可选逐对象观测。"""

from __future__ import annotations

import argparse
from collections import defaultdict
import ctypes
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time


def available_memory_mb() -> int | None:
    try:
        if os.name == "nt":
            class MemoryStatus(ctypes.Structure):
                _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong)] + [
                    (name, ctypes.c_ulonglong) for name in
                    ("total", "available", "page_total", "page_available", "virtual_total", "virtual_available", "extended")
                ]
            status = MemoryStatus()
            status.length = ctypes.sizeof(status)
            if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
                return None
            return status.available // (1024 * 1024)
        for line in pathlib.Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) // 1024
    except (OSError, ValueError, AttributeError):
        pass
    return None


def jobs(_: argparse.Namespace) -> int:
    # 启动时一次性选择，不在编译中途改 jobserver。384 MiB/任务是保守初始
    # 主机预算，不是实测峰值；预留 768 MiB，未知主机回退到原来的最多 4 路。
    available = available_memory_mb()
    cpu_limit = min(8, os.cpu_count() or 1)
    count = min(cpu_limit, max(1, (available - 768) // 384)) if available is not None else min(cpu_limit, 4)
    print(count)
    return 0


def host_key(_: argparse.Namespace) -> int:
    """只读内容身份，隔离 Python ABI 与安装配方；不把文件 mtime 当作版本。"""
    root = pathlib.Path(__file__).resolve().parents[2]
    digest = hashlib.sha256((sys.executable + sys.version + sys.platform).encode("utf-8"))
    for name in ("make/host_tools.mk", "tools/generation/host_tools.py", "tools/generation/requirements-host.txt",
                 "Middlewares/Third_Party/MCUboot/scripts/requirements.txt"):
        digest.update(name.encode("utf-8") + b"\0")
        digest.update((root / name).read_bytes().replace(b"\r\n", b"\n"))
    print(digest.hexdigest()[:24])
    return 0


def start(arguments: argparse.Namespace) -> int:
    from bootstrap_ccache import ccache_version, ensure_ccache

    directory = pathlib.Path(tempfile.mkdtemp(prefix="dima-build-session."))
    (directory / "events").mkdir()
    data = {
        "started": time.monotonic(), "build_dir": str(pathlib.Path(arguments.build_dir).resolve()),
        "jobs": arguments.jobs, "available_mb": available_memory_mb(), "ccache": "", "ccache_version": "",
    }
    if arguments.ccache != "off":
        try:
            executable = ensure_ccache(pathlib.Path(arguments.cache_root))
            if executable is None:
                raise RuntimeError("no compatible ccache found for this host")
            cache = pathlib.Path(arguments.cache_root) / "compiler-cache"
            cache.mkdir(parents=True, exist_ok=True)
            # 二进制可运行且缓存目录可写才算启用；on 失败立即终止，只有
            # 显式 auto 才允许回退，避免用户以为已缓存却一直直接编译。
            with tempfile.TemporaryFile(dir=cache):
                pass
            data["ccache_version"] = ccache_version(executable)
            data["ccache"] = executable.as_posix()
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
            if arguments.ccache == "on":
                print(f"[CACHE] required but unavailable: {error}; install ccache or explicitly set DIMA_CCACHE=auto/off",
                      file=sys.stderr)
                shutil.rmtree(directory)
                return 1
            print(f"[CACHE] unavailable; using GCC directly: {error}", file=sys.stderr)
    (directory / "session.json").write_text(json.dumps(data), encoding="utf-8")
    (directory / "ccache-path").write_text(str(data["ccache"]) + "\n", encoding="utf-8")
    cache_description = f"ccache {data['ccache_version']}" if data["ccache"] else "off"
    print(
        f"[BUILD] jobs={arguments.jobs or 'explicit -j/jobserver'} "
        f"available={data['available_mb']} MiB cache={cache_description}",
        file=sys.stderr, flush=True,
    )
    print(directory.as_posix())
    return 0


def record(label: str, display: str, elapsed: float, returncode: int) -> None:
    """每个事件单独落盘，无共享 JSON 锁；默认快速路径不包装每个编译进程。"""
    directory = os.environ.get("DIMA_BUILD_SESSION")
    if not directory:
        return
    try:
        target = pathlib.Path(directory) / "events" / f"{os.getpid()}-{time.monotonic_ns()}.json"
        target.write_text(json.dumps({
            "label": label, "display": display, "seconds": elapsed, "exit_code": returncode,
        }), encoding="utf-8")
    except OSError as error:
        print(f"[TIMING] cannot record {display}: {error}", file=sys.stderr)


def finish(arguments: argparse.Namespace) -> int:
    directory = pathlib.Path(arguments.session)
    data = json.loads((directory / "session.json").read_text(encoding="utf-8"))
    data["seconds"] = time.monotonic() - data["started"]
    data["exit_code"] = arguments.exit_code
    events = [json.loads(path.read_text(encoding="utf-8")) for path in (directory / "events").glob("*.json")]
    data["events"] = events
    totals: dict[str, float] = defaultdict(float)
    for event in events:
        totals[event["label"]] += event["seconds"]
    print(f"[BUILD] total={data['seconds']:.2f}s exit={arguments.exit_code}", flush=True)
    if totals:
        print("[TIMING] recorded work (parallel durations overlap): " + ", ".join(
            f"{label}={seconds:.2f}s" for label, seconds in sorted(totals.items())
        ), flush=True)
    for event in sorted((item for item in events if item["label"] in {"CC", "CXX"}), key=lambda item: item["seconds"], reverse=True)[:8]:
        print(f"[SLOW] {event['seconds']:.2f}s {event['display']}", flush=True)
    if data["ccache"] and (directory / "ccache.log").is_file():
        # ccache 的会话 stats log 不清零全局计数，避免干扰其他工作树的统计。
        environment = dict(os.environ, CCACHE_STATSLOG=(directory / "ccache.log").as_posix())
        try:
            subprocess.run([data["ccache"], "--show-log-stats"], env=environment, check=False, timeout=5)
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f"[CACHE] statistics unavailable: {error}", file=sys.stderr)
    elif data["ccache"]:
        # Make 已跳过全部编译时不会生成统计日志；这属于正常增量构建，
        # 不应让 ccache 对不存在的文件报错，也不能宣称本轮发生缓存命中。
        print("[CACHE] no compile calls; Make reused existing objects", flush=True)
    report = directory / "summary.json"
    report.write_text(json.dumps(data, indent=2), encoding="utf-8")
    print(f"[TIMING] report={report.as_posix()}", flush=True)
    return 0
