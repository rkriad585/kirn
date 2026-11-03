"""Download and execute the matching Kirn release archive."""

from __future__ import annotations

import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path


def _version() -> str:
    value = os.environ.get("KIRN_VERSION", "0.0.1b3").lstrip("v")
    match = re.fullmatch(r"(\d+\.\d+\.\d+)(?:-?b(\d+))?", value)
    if not match:
        raise RuntimeError(f"unsupported Kirn version: {value}")
    return f"{match.group(1)}-b{match.group(2)}" if match.group(2) else match.group(1)


def _target() -> str:
    system = platform.system().lower()
    machine = platform.machine().lower()
    arch = {"x86_64": "amd64", "amd64": "amd64", "aarch64": "arm64", "arm64": "arm64"}.get(machine)
    if system == "windows" and arch == "amd64":
        return "windows-amd64"
    if system == "linux" and arch in {"amd64", "arm64"}:
        return f"linux-{arch}"
    if system == "darwin" and arch in {"amd64", "arm64"}:
        return f"darwin-{arch}"
    raise RuntimeError(f"unsupported platform: {platform.system()}/{platform.machine()}")


def _binary(cache: Path) -> Path:
    return cache / "bin" / ("kirn.exe" if platform.system().lower() == "windows" else "kirn")


def _download(cache: Path, version: str, target: str) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    suffix = ".zip" if platform.system().lower() == "windows" else ".tar.gz"
    archive_name = f"kirn-{target}{suffix}"
    url = f"https://github.com/{os.environ.get('KIRN_REPO', 'rkriad585/kirn')}/releases/download/v{version}/{archive_name}"
    with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as handle:
        archive = Path(handle.name)
    try:
        urllib.request.urlretrieve(url, archive)
        with tempfile.TemporaryDirectory() as extracted_name:
            extracted = Path(extracted_name)
            if suffix == ".zip":
                with zipfile.ZipFile(archive) as package:
                    package.extractall(extracted)
            else:
                with tarfile.open(archive, "r:gz") as package:
                    package.extractall(extracted)
            roots = list(extracted.iterdir())
            root = roots[0] if len(roots) == 1 and roots[0].is_dir() else extracted
            for entry in root.iterdir():
                destination = cache / entry.name
                if entry.is_dir():
                    shutil.copytree(entry, destination, dirs_exist_ok=True)
                else:
                    shutil.copy2(entry, destination)
    finally:
        archive.unlink(missing_ok=True)
    executable = _binary(cache)
    if not executable.exists():
        raise RuntimeError(f"release v{version} did not contain {executable.name}")
    if platform.system().lower() != "windows":
        executable.chmod(executable.stat().st_mode | 0o111)
    return executable


def main() -> int:
    version = _version()
    cache = Path(os.environ.get("KIRN_PYPI_HOME", Path.home() / ".kirn-pypi" / version))
    executable = _binary(cache)
    if not executable.exists():
        executable = _download(cache, version, _target())
    env = os.environ.copy()
    env["PATH"] = f"{executable.parent}{os.pathsep}{env.get('PATH', '')}"
    return subprocess.run([str(executable), *sys.argv[1:]], env=env).returncode


if __name__ == "__main__":
    raise SystemExit(main())
