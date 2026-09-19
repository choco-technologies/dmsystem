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
| `libsystemd_service_type_t` | enum | `LIBSYSTEMD_SERVICE_TYPE_SIMPLE` (default) / `LIBSYSTEMD_SERVICE_TYPE_ONESHOT` / `LIBSYSTEMD_SERVICE_TYPE_MODULE` - from the unit's `type` key, see [configuration.md](configuration.md#keys). |
| `libsystemd_restart_policy_t` | enum | `LIBSYSTEMD_RESTART_NO` (default) / `LIBSYSTEMD_RESTART_ALWAYS` / `LIBSYSTEMD_RESTART_ON_FAILURE` - from the unit's `restart` key, see [configuration.md](configuration.md#keys). |
| `libsystemd_service_info_t` | struct | `{ const char* unit_name; const char* description; libsystemd_service_type_t type; libsystemd_restart_policy_t restart_policy; libsystemd_service_status_t status; }` - passed to `libsystemd_list()`'s visitor. |
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

### `libsystemd_start_service(const char* unit_name, const char* user_value)`

Looks `unit_name` up in the global registry and starts it. For a `simple`/
`oneshot` unit, spawns it (`Dmod_RunModuleDetached` with the unit's
`exec`/`argc`/`argv`/stream redirections - detached rather than
`Dmod_SpawnModule` so the unit's lifetime is never tied to whichever process
happened to call this). For a `type=module` unit, instead loads and enables
`exec` as a Library module (`Dmod_LoadModuleByName` + `Dmod_EnableModule`) -
no process is spawned; see [configuration.md](configuration.md#typemodule-services-backed-by-a-library-module-not-a-process).

If `unit_name` is not already registered but is `<prefix>@<instance>`-shaped
and `<prefix>@.ini` exists in the last-scanned units directory, it is
instantiated on the fly and added to the registry before being spawned - see
[configuration.md](configuration.md#starting-an-instance-that-was-never-scanned).
This is the only place template instantiation happens outside of
`libsystemd_scan()`/`libsystemd_parse_dir()`. `user_value` (may be `NULL`) is
substituted for `%v` in the template's keys during that on-demand
instantiation only - it has no effect if `unit_name` is already registered.

If the unit's `restart` key is not `no`, also registers a `dmosi` process-exit
callback on the newly spawned process (best-effort - silently skipped if
unsupported on this build/platform) so the unit is automatically respawned if
its process later exits on its own - see [configuration.md](configuration.md#restart-supervision).
Not applicable to a `type=module` unit - there is no process to register a
callback on, so `restart` is simply ignored for it.

For a `simple`/`oneshot` unit:
- `-EINVAL` - `unit_name` was `NULL`.
- `-ENOENT` - no unit with that name, and it could not be instantiated from a
  template either (including "nothing has been scanned yet").
- `-EALREADY` - the unit already has a live process.
- `-ENOSYS` - module spawning is unavailable on this build/platform.
- other negative values are forwarded from `Dmod_RunModuleDetached`.

For a `type=module` unit:
- `-EINVAL` - `unit_name` was `NULL`.
- `-ENOENT` - no unit with that name, and it could not be instantiated from a
  template either; or the module could not be found/loaded.
- `-EALREADY` - the module is already enabled.
- `-EIO` - the module was loaded but could not be enabled (e.g. it is not a
  Library module, or a required module could not be enabled).

### `libsystemd_stop_service(const char* unit_name)`

Looks `unit_name` up and stops it. For a `simple`/`oneshot` unit, kills its
tracked process (`dmosi_process_kill`); if a restart-supervision callback is
registered for the unit, unregisters it first, so a deliberate stop is never
mistaken for a crash that needs restarting. For a `type=module` unit, instead
disables then unloads `exec` (`Dmod_DisableModule` + `Dmod_UnloadModule`) -
disable before unload, since the dmod core refuses to unload a module that is
still enabled.

- `-EINVAL` - `unit_name` was `NULL`.
- `-ENOENT` - no unit with that name.
- `-ESRCH` - the unit exists but has no running process to stop (or, for a
  `type=module` unit, is neither loaded nor enabled).
- `-EIO` - (`type=module` only) disabling or unloading the module failed.

### `libsystemd_notify_main_pid(const char* unit_name, Dmod_Pid_t pid)`

Re-points a unit at another process as its main PID, for units whose `exec`
is only a launcher: it sets something up, starts the real payload, and exits.
After the call the unit tracks `pid`, so `libsystemd_status()` reports it,
`libsystemd_stop_service()` kills it, and its exit is what drives the unit's
`restart` policy. Supervision is moved with it - the exit callback on the
previous process is unregistered and re-registered on `pid`. The previous
process is not killed; it is expected to be the caller, on its way out.

Pass `NULL` as `unit_name` to mean "the unit whose current main PID is the
calling process", which is the usual case and saves a launcher from having
its own unit name plumbed through argv.

The launcher must start the payload with `Dmod_RunModuleDetached()`, not
`Dmod_SpawnModule()`: a spawned child is parented under its spawner, and a
process exiting takes its whole parented subtree with it, so a launcher that
exits right after adopting would kill what it just handed the unit to. See
`dmtty`'s `console` for a worked example.

- `-EINVAL` - `pid` was not positive.
- `-ENOENT` - no unit with that name, or (for a `NULL` `unit_name`) the
  caller is not any unit's tracked main process.
- `-ESRCH` - `pid` does not resolve to a live process.
- `-ENOSYS` - process lookup is not connected on this build/platform.

### `libsystemd_status(const char* unit_name, libsystemd_service_status_t* out_status)`

Fills `*out_status` with the unit's current process state and PID. Reports
`DMOSI_PROCESS_STATE_CREATED`/pid `0` for a unit that was never started, and
`DMOSI_PROCESS_STATE_TERMINATED` for one that was started but whose process
can no longer be found (e.g. it exited on its own).

For a `type=module` unit there is no process/PID at all - `pid` is always
`0`, and `state` is read live from the module's own state (`Dmod_IsModuleEnabled`):
`DMOSI_PROCESS_STATE_RUNNING` while enabled, `DMOSI_PROCESS_STATE_CREATED`
otherwise (covering both "never started" and "stopped" - unlike a
process-backed unit there is no tracked "last" identity to tell those apart).

- `-EINVAL` - `unit_name`/`out_status` was `NULL`.
- `-ENOENT` - no unit with that name.

### `libsystemd_list(libsystemd_visitor_t visitor, void* user_ptr)`

Calls `visitor` once per unit in the current registry (in its current
order), stopping early if `visitor` returns `false`.

- `-EINVAL` - `visitor` was `NULL`.
- `0` - always returned otherwise, including when the registry is
  empty/uninitialized (there is simply nothing to visit).

### `libsystemd_load_rules(const char* rules_dir)`

Loads device-class rules from every `*.ini`/`*.rules` file found anywhere
under `rules_dir`, recursively - see [configuration.md](configuration.md#device-rules). Fully
independent of `libsystemd_scan()`. Replaces any previously loaded rules
(from this or a different directory) - same "full reload" semantics as
`libsystemd_scan()`. Afterwards, retries every device still remembered from
an earlier `libsystemd_notify_device_added()` call against the new rules
(see [Devices reported before rules/units exist yet](configuration.md#devices-reported-before-rulesunits-exist-yet)) -
this is the only way loading rules can start/stop anything by itself.

- `-EINVAL` - `rules_dir` was `NULL`.
- `-ENOENT` - `rules_dir` does not exist / cannot be opened.
- `-ENOMEM` - allocation failed.

### `libsystemd_notify_device_added(const char* device_class, const char* device_name, const char* user_value)`

Resolves `(device_class, device_name)` to a unit name via the rules loaded
by the last `libsystemd_load_rules()` call (`%name` in the matching rule's
`start` value is replaced with `device_name`) and starts it via
`libsystemd_start_service()` - which instantiates it from a template on
demand if it is not already registered, substituting `user_value` (may be
`NULL`) for `%v` in that template's keys.

The device (and `user_value`) is remembered regardless of whether it
resolves/starts right now - drivers commonly report devices before
`libsystemd_scan()`/`libsystemd_load_rules()` have run, so a non-zero return
here does not mean the device was dropped; a later `libsystemd_scan()`/
`libsystemd_load_rules()` call retries it, with the same `user_value`. See
[Devices reported before rules/units exist yet](configuration.md#devices-reported-before-rulesunits-exist-yet).

- `-EINVAL` - `device_class`/`device_name` was `NULL`.
- `-ENOENT` - no rule currently matches `device_class`, or the resolved unit
  could not be found/instantiated yet.
- `-ENOMEM` - allocation failed.
- other negative values are forwarded from `libsystemd_start_service()`.

### `libsystemd_notify_device_removed(const char* device_class, const char* device_name)`

Resolves `(device_class, device_name)` exactly like
`libsystemd_notify_device_added()` and calls `libsystemd_stop_service()` on
the result instead of starting it. Also forgets the device (regardless of
whether it could be resolved/stopped right now), so it is never resurrected
by a later `libsystemd_scan()`/`libsystemd_load_rules()` replay.

- `-EINVAL` - `device_class`/`device_name` was `NULL`.
- `-ENOENT` - no rules loaded, no rule matches `device_class`, or no unit
  with the resolved name is currently registered.
- `-ESRCH` - the resolved unit exists but has no running process to stop.
- `-ENOMEM` - allocation failed while resolving the target.
