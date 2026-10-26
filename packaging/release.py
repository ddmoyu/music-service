"""Prepare and verify release artifacts using only the Python standard library."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TARGETS = {
    "windows-x86_64": ".zip",
    "linux-x86_64": ".tar.gz",
    "linux-arm64": ".tar.gz",
    "macos-x86_64": ".tar.gz",
    "macos-arm64": ".tar.gz",
}


def version():
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    return re.search(r"project\(music_service\s+VERSION\s+(\d+\.\d+\.\d+)", text)[1]


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def checksums(directory):
    files = sorted(p for p in directory.iterdir()
                   if p.is_file() and p.name != "SHA256SUMS")
    if not files or any(p.is_symlink() or "\n" in p.name or "\r" in p.name for p in files):
        raise ValueError("checksum input must contain regular files")
    (directory / "SHA256SUMS").write_text(
        "".join(f"{digest(p)}  {p.name}\n" for p in files), encoding="utf-8")


def verify(directory):
    manifest = directory / "SHA256SUMS"
    if manifest.stat().st_size > 65536:
        raise ValueError("checksum manifest is too large")
    files = {}
    for line in manifest.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([a-f0-9]{64})  ([^\r\n]+)", line)
        if not match:
            raise ValueError("invalid SHA256SUMS line")
        expected, name = match.groups()
        if name in files or name in (".", "..", "SHA256SUMS") or any(c in name for c in "/\\:"):
            raise ValueError("invalid or duplicate checksum filename")
        path = directory / name
        if path.is_symlink() or not path.is_file() or digest(path) != expected:
            raise ValueError(f"checksum mismatch: {name}")
        files[name] = expected
    if not files:
        raise ValueError("empty checksum manifest")
    return files


def assemble(artifacts, output, commit):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("a full source commit is required")
    if output.exists() and any(output.iterdir()):
        raise ValueError("output directory must be empty")
    packages = []
    for target, extension in TARGETS.items():
        directory = artifacts / f"music-service-{target}"
        name = f"music-service-{version()}-{target}{extension}"
        if set(verify(directory)) != {name}:
            raise ValueError(f"unexpected package set for {target}")
        packages.append(directory / name)
    output.mkdir(parents=True, exist_ok=True)
    for package in packages:
        shutil.copyfile(package, output / package.name)
    integration = output / f"music-service-{version()}-frontend-integration.zip"
    with zipfile.ZipFile(integration, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in ("docs/frontend-integration.md", "docs/api/openapi.json",
                     "examples/frontend/client.mjs", "examples/frontend/client.d.mts",
                     "examples/frontend/usage.mjs"):
            archive.write(ROOT / name, name)
    records = [{"name": p.name, "size_bytes": p.stat().st_size, "sha256": digest(p)}
               for p in sorted(output.iterdir())]
    (output / "release-manifest.json").write_text(
        json.dumps({"version": version(), "commit": commit, "files": records},
                   ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    checksums(output)
    print(json.dumps({"version": version(), "commit": commit, "files": records}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("version")
    for command in ("checksums", "verify"):
        sub.add_parser(command).add_argument("directory", type=Path)
    prepare = sub.add_parser("assemble")
    prepare.add_argument("--artifacts", required=True, type=Path)
    prepare.add_argument("--output", required=True, type=Path)
    prepare.add_argument("--commit", required=True)
    tag = sub.add_parser("check-tag")
    tag.add_argument("tag")
    args = parser.parse_args()
    if args.command == "version":
        print(version())
    elif args.command == "check-tag":
        if args.tag != f"v{version()}":
            raise ValueError(f"tag must be v{version()}, got {args.tag!r}")
    elif args.command == "checksums":
        checksums(args.directory)
    elif args.command == "verify":
        print(json.dumps(verify(args.directory), indent=2))
    else:
        assemble(args.artifacts, args.output, args.commit)


if __name__ == "__main__":
    main()
