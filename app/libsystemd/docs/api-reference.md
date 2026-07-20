# libsystemd API reference

`libsystemd` is a Library module - see the [repository README](../../../README.md)
for why that matters and how `systemd`/`service` consume it. Everything below
is declared in [`include/libsystemd.h`](../include/libsystemd.h) and
[`include/libsystemd_types.h`](../include/libsystemd_types.h); full parameter
docs, return codes and usage examples for the implementation live as Doxygen
comments directly in [`src/serviceapi.c`](../src/serviceapi.c) - this page is
a quick-reference summary, not a replacement for those.

## Types

| Type | Kind | Meaning |
|------|------|---------|
| `libsystemd_service_t` | opaque pointer | Handle to one parsed unit. Fields are private to `serviceapi.c`. |
| `libsystemd_services_t` | opaque pointer | Handle to a registry (list) of `libsystemd_service_t`. |
| `libsystemd_service_status_t` | struct | `{ dmosi_process_state_t state; dmosi_process_id_t pid; }` - a unit's current process state and PID. |
| `libsystemd_service_info_t` | struct | `{ const char* unit_name; libsystemd_service_status_t status; }` - passed to `libsystemd_list()`'s visitor. |
| `libsystemd_visitor_t` | function pointer | `bool (*)(const libsystemd_service_info_t* info, void* user_ptr)` - return `false` to stop `libsystemd_list()` early. |

## Functions

All functions return `0` on success and a negative `errno`-style code on
failure unless noted otherwise.

### `libsystemd_scan(const char* path)`

The main entry point, and the only one that needs to be called to bring a
units directory up. Parses every `*.ini` file in `path` (via
`libsystemd_parse_dir()`), resolves `after`/`requires` ordering, stops
whatever the *previous* registry had running, replaces the global registry,
and starts every unit in the new one in order. See
[configuration.md](configuration.md) for the full pipeline.

- `-EINVAL` - `path` was `NULL`.
- `-ENOENT` - `path` does not exist / cannot be opened.
- `-ENOMEM` - allocation failed.

Calling this again later is a full reload: units that disappeared since the
last scan are dropped, and every unit - even ones whose file did not change
- is stopped and restarted.

### `libsystemd_parse_file(const char* file_path, libsystemd_service_t* service)`

Parses one `*.ini` file into a fresh `libsystemd_service_t`. Does not touch
the global registry and does not set the unit's name (see
[configuration.md](configuration.md#unit-naming)) - this is the building
block `libsystemd_parse_dir()` is built on, not something most callers need
directly.

- `-EINVAL` - `file_path`/`service` was `NULL`, or the file has no `exec` key.
- `-ENOMEM` - allocation failed partway through.
- other negative values are `DMINI_ERR_*` codes forwarded from `dmini_parse_file`.

### `libsystemd_parse_dir(const char* dir_path, libsystemd_services_t* services)`

Parses every `*.ini` file directly inside `dir_path` into a fresh registry.
Files that fail to parse are logged and skipped, not treated as a fatal
error for the whole scan. Does not resolve ordering, sort, or start
anything - see `libsystemd_scan()`.

A file named `<prefix>@.ini` is a template and is skipped entirely; a file
named `<prefix>@<instance>.ini` is parsed on top of its template (if one
exists) with `%i`/`%I`/`%p`/`%n`/`%%` expanded - see
[configuration.md](configuration.md#templates).

- `-EINVAL` - `dir_path`/`services` was `NULL`.
- `-ENOENT` - `dir_path` does not exist / cannot be opened.
- `-ENOMEM` - allocation failed.

### `libsystemd_start_service(const char* unit_name)`

Looks `unit_name` up in the global registry and spawns it (`Dmod_SpawnModule`
with the unit's `exec`/`argc`/`argv`/stream redirections).

If `unit_name` is not already registered but is `<prefix>@<instance>`-shaped
and `<prefix>@.ini` exists in the last-scanned units directory, it is
instantiated on the fly and added to the registry before being spawned - see
[configuration.md](configuration.md#starting-an-instance-that-was-never-scanned).
This is the only place template instantiation happens outside of
`libsystemd_scan()`/`libsystemd_parse_dir()`.

- `-EINVAL` - `unit_name` was `NULL`.
- `-ENOENT` - no unit with that name, and it could not be instantiated from a
  template either (including "nothing has been scanned yet").
- `-EALREADY` - the unit already has a live process.
- `-ENOSYS` - module spawning is unavailable on this build/platform.
- other negative values are forwarded from `Dmod_SpawnModule`.

### `libsystemd_stop_service(const char* unit_name)`

Looks `unit_name` up and kills its tracked process (`dmosi_process_kill`).

- `-EINVAL` - `unit_name` was `NULL`.
- `-ENOENT` - no unit with that name.
- `-ESRCH` - the unit exists but has no running process to stop.

### `libsystemd_status(const char* unit_name, libsystemd_service_status_t* out_status)`

Fills `*out_status` with the unit's current process state and PID. Reports
`DMOSI_PROCESS_STATE_CREATED`/pid `0` for a unit that was never started, and
`DMOSI_PROCESS_STATE_TERMINATED` for one that was started but whose process
can no longer be found (e.g. it exited on its own).

- `-EINVAL` - `unit_name`/`out_status` was `NULL`.
- `-ENOENT` - no unit with that name.

### `libsystemd_list(libsystemd_visitor_t visitor, void* user_ptr)`

Calls `visitor` once per unit in the current registry (in its current
order), stopping early if `visitor` returns `false`.

- `-EINVAL` - `visitor` was `NULL`.
- `0` - always returned otherwise, including when the registry is
  empty/uninitialized (there is simply nothing to visit).
