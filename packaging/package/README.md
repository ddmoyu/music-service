# music-service

A backend service for music search, downloads and media, controlled over HTTP. This
package contains only the program and what it needs to run: no development tools,
libraries or runtimes have to be installed. The full documentation is in the source
repository.

```text
bin/music-service[.exe]                   the program
share/music-service/config.example.json  configuration example
share/music-service/licenses/            third-party licenses (THIRD-PARTY.md)
share/music-service/integration/         OpenAPI, frontend guide and JS/TS examples
lib/music-service/                       bundled LLVM runtime (macOS only)
share/music-service/service/             Windows service, systemd or launchd integration
```

## Requirements

| Package | System | Notes |
| --- | --- | --- |
| windows-x86_64 | Windows 10 1809 / Server 2019 or later, x64 | Microsoft runtime linked in; uses the system certificate store |
| linux-x86_64 | glibc at least the one of the build system (printed by `ldd --version` there), x86_64 | libstdc++/libgcc linked in; needs the distribution CA bundle (ca-certificates) |
| linux-arm64 | as above, aarch64 | validated on a native Ubuntu 24.04 ARM64 CI runner; other distributions and NAS devices are not covered |
| macos-arm64 | macOS 15 target, Apple Silicon | LLVM C++ runtime included; see MACOS.md and macos-package.json |
| macos-x86_64 | macOS 15 target, Intel | LLVM C++ runtime included; see MACOS.md and macos-package.json |

Both Linux architectures were validated on Ubuntu 24.04. The tested systems and
remaining distribution limits are recorded in the source repository's
`docs/research/cross-platform-ci.md`. On macOS, keep the complete bin/lib/share layout;
the archive's `macos-package.json` records the actual minimum OS and signing state.
The default ad-hoc signature does not constitute Developer ID signing or notarization.

## Running in the foreground

To connect a third-party frontend, read
share/music-service/integration/docs/frontend-integration.md. The integration files
include a core OpenAPI description and a dependency-free JavaScript client.

```text
music-service --config config.json check-config
music-service --config config.json init      # once; prints where the bootstrap token is
music-service --config config.json serve     # Ctrl+C or SIGTERM stops it cleanly
```

Keep `data_dir` on a local disk outside this directory. Download directories must exist
and be writable by the account that runs the service. `backup --output FILE` copies the
database while the service runs; keep `data_dir/secrets/` together with it.

## As a Windows service

From an elevated PowerShell in the unpacked directory:

```powershell
.\share\music-service\service\install-service.ps1 -DownloadDir D:\Music
```

The program is copied to `%ProgramFiles%\music-service` (`-InstallDir` to change it;
service accounts cannot read a package left in a user profile). The service runs as the
virtual account `NT SERVICE\music-service`, keeps its configuration and data under
`%ProgramData%\music-service`, logs to
`data\logs\music-service.log` and reports startup errors to the Application event log.
`uninstall-service.ps1` removes the service and the program (add `-RemoveData` to delete
configuration and data; downloads are never deleted).

## As a systemd service

```bash
sudo ./share/music-service/service/install.sh /srv/music
```

The program goes to `/opt/music-service`, the configuration to
`/etc/music-service/config.json`, data to `/var/lib/music-service`; it runs as the system
user `music-service` with a hardened unit and logs to the journal
(`journalctl -u music-service`).

## On macOS

Follow `share/music-service/MACOS.md` for foreground setup and the per-user launchd
agent. Use `config.macos.example.json` instead of the Linux-oriented default example;
it uses paths relative to the configuration file and enables rotating file logs.
Homebrew, LLVM and Python are build-time tools and are not required to run the package.
