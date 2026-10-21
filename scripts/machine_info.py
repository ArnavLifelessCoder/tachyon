"""Collect machine details recorded next to every committed result."""

import json
import os
import platform
import re
import shutil
import subprocess
import sys


def _run(command):
    try:
        return subprocess.run(command, capture_output=True, text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def _windows():
    script = (
        "$c = Get-CimInstance Win32_Processor | Select-Object -First 1;"
        "$m = Get-CimInstance Win32_ComputerSystem;"
        "$o = Get-CimInstance Win32_OperatingSystem;"
        "[pscustomobject]@{cpu=$c.Name.Trim(); cores=$c.NumberOfCores;"
        "threads=$c.NumberOfLogicalProcessors; max_mhz=$c.MaxClockSpeed;"
        "ram_bytes=$m.TotalPhysicalMemory; os=$o.Caption; build=$o.BuildNumber;"
        "power_plan=(powercfg /getactivescheme)} | ConvertTo-Json"
    )
    data = json.loads(_run(["powershell", "-NoProfile", "-Command", script]) or "{}")
    return {
        "cpu": data.get("cpu"),
        "cores": data.get("cores"),
        "logical_processors": data.get("threads"),
        "max_mhz": data.get("max_mhz"),
        "ram_bytes": data.get("ram_bytes"),
        "os": f"{data.get('os')} build {data.get('build')}",
        "power_plan": data.get("power_plan"),
        "kernel": platform.version(),
    }


def _linux():
    lscpu = _run(["lscpu"])
    field = lambda name: (re.search(rf"^{name}:\s*(.+)$", lscpu, re.M) or [None, None])[1]  # noqa: E731
    flags = field("Flags") or ""
    meminfo = open("/proc/meminfo").read() if os.path.exists("/proc/meminfo") else ""
    ram = re.search(r"MemTotal:\s+(\d+) kB", meminfo)
    return {
        "cpu": field("Model name"),
        "cores": os.cpu_count(),
        "ram_bytes": int(ram.group(1)) * 1024 if ram else None,
        "os": _run(["uname", "-a"]),
        "kernel": platform.release(),
        "constant_tsc": "constant_tsc" in flags.split(),
        "nonstop_tsc": "nonstop_tsc" in flags.split(),
    }


def compiler():
    if sys.platform == "win32":
        banner = _run(["cmd", "/c", "cl 2>&1"]).splitlines()
        return banner[0] if banner else "cl (not on PATH)"
    cxx = os.environ.get("CXX", "c++")
    return _run([cxx, "--version"]).splitlines()[0] if shutil.which(cxx) else cxx


def collect(flags=None):
    info = _windows() if sys.platform == "win32" else _linux()
    info.update(
        python=platform.python_version(),
        compiler=compiler(),
        flags=flags,
        machine=platform.machine(),
    )
    return info


if __name__ == "__main__":
    print(json.dumps(collect(), indent=2))
