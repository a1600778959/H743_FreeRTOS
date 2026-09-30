#!/usr/bin/env python3
"""Windows 使用固定 ccache，Linux/WSL 使用兼容的系统 ccache。"""

from __future__ import annotations

import hashlib
import io
import os
import pathlib
import platform
import re
import shutil
import subprocess
import tempfile
import urllib.error
import urllib.request
import zipfile

VERSION = "4.11.3"
ARCHIVE = f"ccache-{VERSION}-windows-x86_64.zip"
ARCHIVE_SIZE = 1642304
ARCHIVE_SHA256 = "bfd031cad091b7db7e68c3303be542b0f7fee7a3e716d76ec6f7e6c7ef4b3526"
EXECUTABLE_SHA256 = "e67407fc24a1ef04bb0368a2d63004879cbd46ae157ca75eec94ae5bddc5fb91"


def ccache_version(executable: pathlib.Path) -> str:
    """核对所需 4.x 能力下限，不能把 Windows 固定版本当成系统工具唯一版本。"""
    result = subprocess.run(
        [str(executable), "--version"], capture_output=True, text=True,
        check=False, timeout=5,
    )
    match = re.match(r"ccache version ((\d+)\.(\d+)(?:\.\d+)?)(?:\s|$)", result.stdout)
    if result.returncode != 0 or not match or (int(match[2]), int(match[3])) < (4, 5):
        raise RuntimeError(f"{executable}: ccache >= 4.5 is required")
    return match[1]


def ensure_ccache(cache_root: pathlib.Path) -> pathlib.Path | None:
    """只安装已核对的 Windows 二进制；系统工具沿用 PATH，不放宽缓存正确性。"""
    if os.name != "nt":
        executable = shutil.which("ccache")
        if executable:
            path = pathlib.Path(executable)
            ccache_version(path)
            return path
        return None
    if platform.machine().lower() not in {"amd64", "x86_64"}:
        return None
    executable = cache_root / "ccache" / VERSION / "windows-x86_64" / "ccache.exe"
    if executable.is_file() and hashlib.sha256(executable.read_bytes()).hexdigest() == EXECUTABLE_SHA256:
        return executable
    url = f"https://github.com/ccache/ccache/releases/download/v{VERSION}/{ARCHIVE}"
    failures: list[str] = []
    for address in (url, "https://ghfast.top/" + url):
        try:
            request = urllib.request.Request(address, headers={"User-Agent": "dima-rover-host-tools/1"})
            with urllib.request.urlopen(request, timeout=10) as response:
                archive = response.read(ARCHIVE_SIZE + 1)
            if len(archive) != ARCHIVE_SIZE or hashlib.sha256(archive).hexdigest() != ARCHIVE_SHA256:
                raise ValueError("ccache archive size/SHA-256 mismatch")
            # 只读取确定的 exe 成员，不展开归档路径或在项目内增加第三方文件。
            with zipfile.ZipFile(io.BytesIO(archive)) as source:
                data = source.read(f"ccache-{VERSION}-windows-x86_64/ccache.exe")
            if hashlib.sha256(data).hexdigest() != EXECUTABLE_SHA256:
                raise ValueError("ccache executable SHA-256 mismatch")
            executable.parent.mkdir(parents=True, exist_ok=True)
            descriptor, temporary = tempfile.mkstemp(dir=executable.parent, prefix=".ccache-")
            try:
                with os.fdopen(descriptor, "wb") as output:
                    output.write(data)
                os.replace(temporary, executable)
            finally:
                pathlib.Path(temporary).unlink(missing_ok=True)
            return executable
        except (OSError, urllib.error.URLError, ValueError, KeyError, zipfile.BadZipFile) as error:
            failures.append(str(error))
    raise RuntimeError("; ".join(failures))
