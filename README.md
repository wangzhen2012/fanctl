# fanctl

A macOS menu bar utility for monitoring and controlling fan speed on Apple Silicon Macs.

> This project writes AppleSMC fan controls. Use it only on hardware you understand. Incorrect settings can affect cooling and system stability.

## Features

- Read fan speed, limits, mode, and target speed through `fanctl-smc`.
- Read supported Apple Silicon temperature sensors.
- Apply a configurable temperature-to-RPM rule with EMA smoothing, hysteresis, and write throttling.
- Display temperature and RPM in a menu bar item.
- Restore automatic system fan control with `fanctl-smc reset`.

## Requirements

- macOS on Apple Silicon. The SMC key layout is hardware and OS dependent.
- Xcode Command Line Tools (`clang`).
- Python 3.10 or newer.
- Python packages: `rumps`, `pyobjc-core`, `pyobjc-framework-Cocoa`, and `pyobjc-framework-libdispatch`.
- Administrator privileges for fan writes. Reading status does not require root.

The project is not tested on Intel Macs or non-macOS systems.

## Build

```bash
make
./fanctl-smc fans
./fanctl-smc temp
```

`make check` runs C syntax checking and Python bytecode compilation without changing fan state.

## Run The Menu Bar App

Install the Python dependencies in a virtual environment, then run:

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install rumps pyobjc-core pyobjc-framework-Cocoa pyobjc-framework-libdispatch
python fanctl_menubar.py
```

The app stores its user configuration at:

```text
~/Library/Application Support/fanctl/config.json
```

Set `FANCTL_TOOL` when the compiled helper is located elsewhere:

```bash
FANCTL_TOOL=/absolute/path/to/fanctl-smc python fanctl_menubar.py
```

## Manual Fan Control

```bash
sudo ./fanctl-smc fan -m 1 -v 2000
sudo ./fanctl-smc reset
```

A production installation should use a narrowly scoped `sudoers` rule for this exact executable. Do not grant broad passwordless access to a writable script or directory. The repository intentionally does not install or modify `sudoers`, LaunchAgents, or other system services automatically.

## Development Checks

```bash
make check
make clean
```

The GitHub Actions workflow runs the same non-invasive checks on macOS. Hardware behavior must be verified separately on the target Mac; CI cannot prove SMC read/write behavior.

## Project Layout

- `fanctl-smc.c`: native AppleSMC command-line helper.
- `fanctl_menubar.py`: menu bar application and temperature control loop.
- `mini_test.py`: local menu bar UI smoke-test helper.
- `VERSION`: current project version.
- `CHANGELOG.md`: user-visible changes.
- `VERSIONING.md`: version and baseline policy.

## License

MIT. See [LICENSE](LICENSE).
