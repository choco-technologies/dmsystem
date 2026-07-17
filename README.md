# dmsystem

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

dmsystem DMOD application module.

## Description

`dmsystem` is an init/service-manager module for DMOD, similar in spirit to
`systemd` on Linux. It is started with the path to a **directory** of *unit*
files (managed services) - there is no single master config to hand-edit or
regenerate: each service simply drops its own unit file into that directory.
dmsystem scans the directory, parses every unit file with
[dmini](https://github.com/choco-technologies/dmini), builds a dependency
tree from each unit's `after`/`requires` keys, starts every unit in
dependency order via `Dmod_SpawnModule`/`Dmod_RunModule`, and then supervises
the long-running ones using [dmosi](https://github.com/choco-technologies/dmosi)'s
process API until they all terminate.

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
└── migrate-db.ini
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
# migrate-db.ini
description=One-shot database migration
exec=dbmigrate
after=networking
type=oneshot
```

Recognized keys:

| Key           | Default    | Meaning |
|---------------|------------|---------|
| `exec`        | (required) | Module name to run/spawn |
| `description` | unit name  | Human-readable description, used in logs |
| `args`        | (empty)    | Whitespace-separated extra arguments passed to `exec` |
| `type`        | `simple`   | `simple` (long-running, spawned) or `oneshot` (run to completion) |
| `after`       | (empty)    | `;`/`,`/whitespace-separated unit names that must start (or complete, if `oneshot`) before this one |
| `requires`    | (empty)    | Same ordering as `after`, plus: if the dependency fails, this unit is skipped |
| `restart`     | `no`       | `no` or `always` - whether the supervise loop respawns a terminated `simple` unit |
| `stdin`       | (unset)    | Path to a file to redirect the unit's stdin from |
| `stdout`      | (unset)    | Path to a file to redirect the unit's stdout to |
| `stderr`      | (unset)    | Path to a file to redirect the unit's stderr to |
| `stdlog`      | (unset)    | Path to a file to redirect the unit's stdlog (DMOD's own log stream) to |

An unset stream key leaves that stream at whatever default the spawned module
would otherwise get; `stdout`/`stderr`/`stdlog` may point at the same path (as
in the `webserver.ini` example above) to interleave everything into one file.

A unit with dependency-cycle involvement is skipped and logged as such
instead of blocking the rest of the system from starting.

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

## Usage

```bash
dmod_loader /path/to/dmsystem.dmf /path/to/units-directory
```

### Controlling units with `service`

`service` is a small `systemctl`/`service`-alike CLI, built alongside dmsystem,
for inspecting and controlling the units a running dmsystem is managing:

```bash
dmod_loader /path/to/service.dmf list                 # name/type/exec of every unit
dmod_loader /path/to/service.dmf status               # live state/pid/exit code of every unit
dmod_loader /path/to/service.dmf status webserver      # one unit
dmod_loader /path/to/service.dmf stop webserver
dmod_loader /path/to/service.dmf start webserver
dmod_loader /path/to/service.dmf restart webserver
```

`service` does not talk to dmsystem over any socket or file - it calls
dmsystem_core's query/control API directly. Because DMOD loads a given
Library module at most once and shares that single instance across every
Application that requires it, `service` (which requires dmsystem_core just
like dmsystem does) reaches the exact in-memory unit list dmsystem's
supervise loop is managing. If dmsystem is not currently running, `service`
simply reports zero units.

`start`/`stop`/`restart` act only on the named unit - they do not cascade to
its dependencies. A unit stopped this way is marked `stopped` and is not
auto-restarted even if it declares `restart=always` (that only applies to a
unit terminating on its own).

## Documentation

See the `docs/` directory:

- **[api-reference.md](docs/api-reference.md)** - Command-line usage

View documentation using `dmf-man dmsystem`.

## Project Structure

dmsystem is split across four DMOD modules, each its own loadable `.dmf`:

- **dmsystem** (`src/`) - the executable entry point. Parses argv and calls
  `dmsystem_core_run()`.
- **dmsystem_core** (`lib/dmsystem_core/`) - orchestration: scans the units directory
  and parses each unit file (via [dmini](https://github.com/choco-technologies/dmini)),
  asks dmsystem_unit for the dependency order, starts/supervises units (via
  [dmosi](https://github.com/choco-technologies/dmosi)). Exposes `dmsystem_core_run`
  plus a query/control API (`dmsystem_core_get_unit_count`, `..._get_unit_status`,
  `..._find_unit_status`, `..._start_unit`, `..._stop_unit`, `..._restart_unit`) used
  by `service`.
- **dmsystem_unit** (`lib/dmsystem_unit/`) - the unit model and dependency-graph
  ordering (`dmsystem_unit_list_add`, `dmsystem_unit_topo_sort`, ...). Has no
  dependency on dmini/dmosi, so it can be loaded and tested standalone - this is
  what `tests/dmsystem_test.c` links against and calls as real, dynamically
  resolved DMOD module API, rather than embedding its source into the test.
- **service** (`service/`) - the `systemctl`-alike CLI described above. Calls
  dmsystem_core's query/control API directly; does not talk to dmsystem itself.

```
dmsystem/
├── docs/                          # Documentation (markdown format)
├── lib/
│   ├── dmsystem_unit/
│   │   ├── CMakeLists.txt
│   │   ├── dmsystem_unit.dmr
│   │   ├── dmsystem_unit_types.h  # Plain unit/list/status struct + enum definitions
│   │   ├── dmsystem_unit.c/.h     # Unit model API (list add/find, argv/dep parsing)
│   │   └── dmsystem_graph.c/.h    # Dependency-tree topological sort API
│   └── dmsystem_core/
│       ├── CMakeLists.txt
│       ├── dmsystem_core.dmr
│       ├── dmsystem_core.c/.h     # dmsystem_core_run + service's query/control API
│       ├── dmsystem_config.c/.h   # Units directory scan -> unit list (via dmini)
│       └── dmsystem_proc.c/.h     # Dmod_SpawnModule + dmosi wrapper
├── service/
│   ├── CMakeLists.txt
│   ├── service.dmr
│   └── service.c                  # `service status|start|stop|restart [unit]`
├── src/
│   └── dmsystem.c                 # Entry point (argv parsing, dmod_preinit)
├── tests/
│   ├── CMakeLists.txt
│   └── dmsystem_test.c
├── CMakeLists.txt
├── dmsystem.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak 

## License

MIT
