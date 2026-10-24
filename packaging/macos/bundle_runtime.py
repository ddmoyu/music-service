#!/usr/bin/env python3
"""Bundle only the LLVM runtimes already linked by the macOS executable.

Build-time only: Python's standard library plus Apple's existing developer tools.
Unexpected non-system libraries fail packaging instead of becoming new dependencies.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys


RUNTIME = re.compile(r"lib(?:c\+\+(?:abi)?|unwind)(?:\.\d+)*\.dylib\Z")
SYSTEM = ("/usr/lib/", "/System/Library/")


def run(*args):
    return subprocess.run(
        [str(arg) for arg in args], check=True, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120,
    ).stdout


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def inspect(path):
    commands = run("/usr/bin/otool", "-l", path)
    identity = re.search(r"cmd LC_ID_DYLIB\s+cmdsize \d+\s+name (.*?) \(offset", commands)
    own_name = identity.group(1) if identity else None
    dependencies = [
        line.strip().split(" (compatibility version", 1)[0]
        for line in run("/usr/bin/otool", "-L", path).splitlines()[1:]
        if " (compatibility version" in line
    ]
    rpaths = re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset", commands)
    minimum = re.search(r"cmd LC_BUILD_VERSION\s+cmdsize \d+\s+platform (?:1|MACOS)\s+minos ([\d.]+)", commands)
    if not minimum:
        minimum = re.search(r"cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version ([\d.]+)", commands)
    if not minimum:
        raise ValueError(f"No macOS deployment version in {path}")
    return [dep for dep in dependencies if dep != own_name], rpaths, minimum.group(1)


def expand_path(value, loader, executable):
    for variable, directory in (("@loader_path/", loader.parent), ("@executable_path/", executable.parent)):
        if value.startswith(variable):
            return directory / value[len(variable):]
    if Path(value).is_absolute():
        return Path(value)
    raise ValueError(f"Unsupported runtime search path: {value}")


def resolve_dependency(name, loader, executable, rpaths, executable_rpaths, llvm_root):
    if not RUNTIME.fullmatch(Path(name).name):
        raise ValueError(f"Unreviewed non-system runtime: {name}")
    if name.startswith("@rpath/"):
        paths = [(item, loader) for item in rpaths] + [(item, executable) for item in executable_rpaths]
        candidates = [expand_path(item, owner, executable) / name[len("@rpath/"):] for item, owner in paths]
    else:
        candidates = [expand_path(name, loader, executable)]
    for candidate in candidates:
        if candidate.is_file():
            resolved = candidate.resolve(strict=True)
            if not resolved.is_relative_to(llvm_root):
                raise ValueError(f"Runtime is outside the selected LLVM installation: {name}")
            return resolved
    raise ValueError(f"Cannot resolve {name} used by {loader}")


def bundle(prefix, llvm_root, identity):
    executable = prefix / "bin/music-service"
    if not executable.is_file():
        raise ValueError(f"Missing installed executable: {executable}")
    compiler_version = run(llvm_root / "bin/clang++", "--version").splitlines()[0]
    if not re.search(r"clang version 20\.", compiler_version):
        raise ValueError(f"Expected the existing LLVM 20 toolchain, got {compiler_version}")
    library_dir = prefix / "lib/music-service"
    library_dir.mkdir(parents=True, exist_ok=True)
    _, executable_rpaths, _ = inspect(executable)
    pending = [(executable, executable)]
    origins = {}
    images = []
    versions = []
    while pending:
        source, installed = pending.pop(0)
        dependencies, rpaths, minimum = inspect(source)
        versions.append(minimum)
        changes = []
        for dependency in dependencies:
            if dependency.startswith(SYSTEM):
                continue
            resolved = resolve_dependency(dependency, source, executable, rpaths, executable_rpaths, llvm_root)
            name = Path(dependency).name
            if name in origins and origins[name] != resolved:
                raise ValueError(f"Conflicting runtime basename: {name}")
            if name not in origins:
                if len(origins) >= 8:
                    raise ValueError("Unexpected number of LLVM runtime libraries")
                origins[name] = resolved
                destination = library_dir / name
                shutil.copyfile(resolved, destination)
                destination.chmod(0o755)
                pending.append((resolved, destination))
            relative = "../lib/music-service/" if installed == executable else ""
            changes.extend(["-change", dependency, f"@loader_path/{relative}{name}"])
        # The bundle has direct loader-relative references, so no build RPATH is needed.
        for rpath in dict.fromkeys(rpaths):
            changes.extend(["-delete_rpath", rpath])
        if installed != executable:
            changes.extend(["-id", f"@rpath/{installed.name}"])
        if changes:
            run("/usr/bin/install_name_tool", *changes, installed)
        run("/usr/bin/strip", "-S", installed)
        images.append(installed)
    if not any(name.startswith("libc++.") for name in origins):
        raise ValueError("The package did not include the required LLVM libc++ runtime")

    # Sign dependencies before the executable; do not mutate Mach-O files afterwards.
    for installed in sorted(images, key=lambda path: path == executable):
        signing = ["/usr/bin/codesign", "--force", "--sign", identity]
        if identity != "-":
            signing += ["--options", "runtime", "--timestamp"]
        run(*signing, installed)
        run("/usr/bin/codesign", "--verify", "--strict", installed)
        dependencies, rpaths, _ = inspect(installed)
        if rpaths:
            raise ValueError(f"Unexpected package RPATH: {installed}")
        for dependency in dependencies:
            if dependency.startswith(SYSTEM):
                continue
            if not dependency.startswith("@loader_path/"):
                raise ValueError(f"Non-relocatable package dependency: {dependency}")
            resolved = expand_path(dependency, installed, executable).resolve(strict=True)
            if not resolved.is_relative_to(library_dir):
                raise ValueError(f"Dependency escapes the package runtime directory: {dependency}")

    metadata = {
        "schema": 1,
        "architecture": run("/usr/bin/lipo", "-archs", executable).strip(),
        "minimum_macos": max(versions, key=lambda value: tuple(int(part) for part in value.split("."))),
        "compiler": compiler_version,
        "signing": "ad-hoc" if identity == "-" else "configured-identity",
        "notarized": False,
        "files": [
            {"path": str(path.relative_to(prefix)), "bytes": path.stat().st_size,
             "sha256": sha256(path)}
            for path in sorted(images)
        ],
    }
    target = prefix / "share/music-service/macos-package.json"
    target.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(metadata, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", required=True, type=Path)
    parser.add_argument("--llvm-root", required=True, type=Path)
    parser.add_argument("--identity", default="-")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("macOS packaging requires a native macOS build host")
    bundle(args.prefix.resolve(strict=True), args.llvm_root.resolve(strict=True), args.identity)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"macOS packaging failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stderr, file=sys.stderr)
        sys.exit(1)
