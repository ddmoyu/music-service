# music-service

A backend service for music search, downloads and media, controlled over HTTP. This
package contains only the program and what it needs to run: no development tools,
libraries or runtimes have to be installed. The full documentation is in the source
repository.

```text
bin/music-service[.exe]                   the program
share/music-service/config.example.json  configuration example
share/music-service/licenses/            third-party licenses (THIRD-PARTY.md)
share/music-service/service/             Windows service or systemd integration
```

## Requirements

| Package | System | Notes |
| --- | --- | --- |
| windows-x86_64 | Windows 10 1809 / Server 2019 or later, x64 | Microsoft runtime linked in; uses the system certificate store |
| linux-x86_64 | glibc at least the one of the build system (printed by `ldd --version` there), x86_64 | libstdc++/libgcc linked in; needs the distribution CA bundle (ca-certificates) |
| linux-arm64 | as above, aarch64 | built only where an arm64 build host is available; not a supported package until verified on the target |

## Running in the foreground

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
