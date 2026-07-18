# dmsystem

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

An init/service-manager stack for DMOD, similar in spirit to `systemd` on
Linux.

## Modules in this repository

This repository builds three DMOD modules out of `app/`:

| Module | Type | Role |
|--------|------|------|
| [`libsystemd`](app/libsystemd) | Library | Owns the unit registry and all the logic: parsing unit files, resolving `requires`/`after` order, starting/stopping/querying units. See [app/libsystemd/docs](app/libsystemd/docs) for its API. |
| [`systemd`](app/systemd) | Application | Thin daemon entry point: takes a units directory, calls into `libsystemd` to scan and start everything. |
| [`service`](app/service) | Application | Thin `systemctl`/`service`-alike CLI for inspecting and controlling the units `libsystemd` is currently tracking. |

`libsystemd` has to be a **Library** module, not an Application: DMOD's
loader only allows a Library to be enabled as another module's required
dependency (`Dmod_Enable` rejects anything else - see
`dmod/src/system/dmod_system.c`), and both `systemd` and `service` - plus
`libsystemd`'s own tests - need to call into it. Because DMOD loads a given
Library module at most once per process and shares that single instance
across every module that requires it, `service` (which also requires
`libsystemd`) reaches the exact in-memory unit registry `systemd` populated
with its last scan, **as long as both run in the same process** (e.g. the
same test binary, or a host loader that keeps the module loaded across
runs). Two independently invoked `dmod_loader` processes each get their own
fresh `libsystemd` instance - if `service` is run as a separate process from
`systemd`, it simply sees an empty registry.

## Description

`libsystemd` is started (via `systemd`) with the path to a **directory** of
*unit* files (managed services) - there is no single master config to
hand-edit or regenerate: each service simply drops its own unit file into
that directory. It scans the directory, parses every unit file with
[dmini](https://github.com/choco-technologies/dmini), builds a dependency
order from each unit's `after`/`requires` keys, and starts every unit in that
order via `Dmod_SpawnModule`.

### Configuration format

Every `*.ini` file directly inside the units directory is one unit, named
after its filename with the extension stripped (e.g. `webserver.ini` becomes
the unit `webserver`). Subdirectories and non-`.ini` entries are ignored; the
directory is not searched recursively. A unit file's keys are read from its
global section - no `[section]` header is needed, since the file itself is
the unit:

```
/etc/dmsystem/units/
├── networking.ini
├── webserver.ini
└── monitoring.ini
```

```ini
# networking.ini
description=Bring up network interfaces
exec=dmnetd
type=simple
```

```ini
# webserver.ini
description=HTTP server
exec=dmhttpd
args=--port 8080
after=networking
requires=networking
type=simple
restart=always
stdout=/var/log/webserver.log
stderr=/var/log/webserver.log
```

```ini
# monitoring.ini
description=Watches networking and the webserver
exec=dmmonitor
after=networking,webserver
requires=networking,webserver
type=simple
```

Runnable copies of these three live in
[`app/libsystemd/examples/`](app/libsystemd/examples) and double as fixtures
for `libsystemd`'s own test suite.

Recognized keys:

| Key           | Default    | Status | Meaning |
|---------------|------------|--------|---------|
| `exec`        | (required) | Implemented | Module name to run/spawn |
| `args`        | (empty)    | Implemented | Whitespace-separated extra arguments passed to `exec` |
| `after`       | (empty)    | Implemented | One or more unit names that must start before this one - separate multiple names with `,` and/or whitespace (freely mixed); no fixed cap |
| `requires`    | (empty)    | Implemented (same as `after`) | Same syntax as `after`. Currently treated identically - there is no separate "skip on dependency failure" behavior yet |
| `stdin`       | (unset)    | Implemented | Path to a file to redirect the unit's stdin from |
| `stdout`      | (unset)    | Implemented | Path to a file to redirect the unit's stdout to |
| `stderr`      | (unset)    | Implemented | Path to a file to redirect the unit's stderr to |
| `description` | unit name  | **Not yet implemented** | Parsed by no one - the key is free to set but currently has no effect (not logged, not surfaced by `service`) |
| `type`        | `simple`   | **Not yet implemented** | Key is not read at all yet - every unit is started the same way (spawned via `Dmod_SpawnModule`); there is no `oneshot` run-to-completion behavior |
| `restart`     | `no`       | **Not yet implemented** | Key is not read at all yet - there is no supervise loop, so a unit that exits on its own is simply left stopped |
| `stdlog`      | (unset)    | **Not yet implemented** | Only `stdin`/`stdout`/`stderr` are wired up today |

An unset stream key leaves that stream at whatever default the spawned module
would otherwise get; `stdout`/`stderr` may point at the same path (as in the
`webserver.ini` example above) to interleave both into one file.

Dependency ordering is resolved with a bounded relaxation pass (each unit's
start order is pushed past everything it requires/comes after, repeated
until nothing changes or the list has been fully walked once per unit). A
dependency cycle does not crash or hang anything - it simply stops
propagating once the pass budget is exhausted - but unlike a real cycle
detector it is not currently reported or logged as an error.

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

This produces `build/dmf/libsystemd.dmf`, `build/dmf/systemd.dmf` and
`build/dmf/service.dmf`, plus a `test_<module>.dmf` for each one's test
suite.

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

## Usage

```bash
dmod_loader /path/to/systemd.dmf --args "/path/to/units-directory"
```

### Controlling units with `service`

`service` is a small `systemctl`/`service`-alike CLI, built alongside
`systemd`, for inspecting and controlling the units a running `systemd` is
managing:

```bash
service list                  # every unit systemd currently knows about
service status                # same as `list`
service status webserver      # one unit's state/pid
service stop webserver
service start webserver
service restart webserver
```

`service` does not talk to `systemd` over any socket or file - it calls
`libsystemd`'s control API directly (see "Modules in this repository"
above for why this only sees a populated registry when both run in the same
process).

`start`/`stop`/`restart` act only on the named unit - they do not cascade to
its dependencies.

## Author

Patryk Kubiak 

## License

MIT License - see LICENSE file for details
