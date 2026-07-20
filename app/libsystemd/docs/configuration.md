# Unit file format

A unit file is a plain `*.ini` file with no `[section]` header - every key
is read from the file's global section, via
[dmini](https://github.com/choco-technologies/dmini). See
[`app/libsystemd/examples/`](../examples) for three runnable examples
(`networking.ini`, `webserver.ini`, `monitoring.ini`), which double as
fixtures for the test suite.

## Unit naming

A unit's name is derived purely from its file name: the `.ini` suffix is
stripped and the rest is used verbatim (`webserver.ini` → unit `webserver`).
This happens in `libsystemd_parse_dir()`, not in `libsystemd_parse_file()` -
a service parsed directly via `libsystemd_parse_file()` has no unit name
until its caller assigns one.

The scanned directory is walked recursively: every `.ini` file anywhere
under it is considered, no matter how deeply nested in subdirectories.
Templates (`<prefix>@.ini`) are only merged with instances found in the same
subdirectory - they do not apply across subdirectory boundaries. Everything
else (non-`.ini` files) is silently skipped.

## Keys

| Key        | Required | Meaning |
|------------|----------|---------|
| `exec`     | yes      | Module name (or file path) passed to `Dmod_SpawnModule` when the unit is started. Also becomes `argv[0]`. |
| `args`     | no       | Extra arguments for `exec`, split on runs of spaces/tabs. Each token becomes one more `argv` entry after `argv[0]`. |
| `after`    | no       | Unit names this unit must be ordered after. Multiple names are split on `,` and/or whitespace (freely mixed). |
| `requires` | no       | Same syntax as `after`. Currently resolved identically to `after` - both just push this unit's `starting_order` past the referenced units'. |
| `stdin`    | no       | Path bound to `DMOD_STDIN` in the spawned process's stream redirections. |
| `stdout`   | no       | Path bound to `DMOD_STDOUT`. |
| `stderr`   | no       | Path bound to `DMOD_STDERR`. |
| `stdlog`   | no       | Path bound to `DMOD_STDLOG` - a separate, platform-configurable logging stream that defaults to the same target as `DMOD_STDOUT` unless the platform overrides `Dmod_GetStdLogFile()`. |
| `description` | no   | Free-form text, surfaced (not parsed/interpreted) via `libsystemd_service_info_t.description` in `libsystemd_list()`, and printed by `service list`/`service status`. |
| `type`     | no       | `simple` (default) or `oneshot` - see [Service type](#service-type) below. |
| `restart`  | no       | `no` (default), `always` or `on-failure` - see [Restart supervision](#restart-supervision) below. |

A unit file with no `exec` key fails to parse (`libsystemd_parse_file()`
returns `-EINVAL`); `libsystemd_parse_dir()` logs that failure and skips the
file rather than aborting the whole scan.

An unrecognized `type`/`restart` value is logged via `DMOD_LOG_WARN()` and
treated as the default (`simple`/`no`) - an unknown value must never silently
enable restart supervision.

### Service type

The `type` key only affects how a process's own (non-killed) exit is logged,
not whether/how it is started - every unit is spawned the same way, via
`Dmod_SpawnModule`:

- `simple` (default) - the process is expected to keep running until
  explicitly stopped. An exit it was not killed for is logged as a warning.
- `oneshot` - the process is expected to run to completion and exit on its
  own. A clean exit (status `0`) is logged as informational, not a warning.

### Restart supervision

The `restart` key opts a unit into automatic restart when its process exits
**on its own** (never when it is stopped via `service stop`/
`libsystemd_stop_service()` - see below):

- `no` (default) - never restart automatically; an exited unit is simply left
  stopped, exactly like before this key existed.
- `always` - restart unconditionally, regardless of exit status.
- `on-failure` - restart only if the process exited with a non-zero status.

This is implemented with `dmosi`'s process exit-callback API
(`dmosi_process_register_exit_callback()`/`dmosi_process_unregister_exit_callback()`),
not a polling supervise loop: `libsystemd_start_service_internal()` registers
a callback on the freshly spawned process whenever `restart` is not `no`, and
the callback re-spawns the unit (via the same internal start path, so a
restart also re-registers itself) if the policy calls for it. Registration is
best-effort - if `dmosi_process_register_exit_callback` is not connected on a
given build/platform, or registration itself fails, the unit simply behaves
as if `restart=no` (no supervision, same as before this feature existed) -
`libsystemd_start_service()`'s return value is unaffected either way.

Because the callback fires for *any* process exit, `libsystemd_stop_service()`
unregisters it before killing the process - otherwise a `restart=always`
unit would immediately respawn itself in response to a deliberate `service
stop`. `libsystemd_scan()`'s "stop the previous generation" pass and
`libsystemd_serviceapi_deinit()` (module unload) go through the same
stop path, so neither leaves a stray restart behind either.

The callback may run on whatever thread/context the `dmosi` backend detects
process termination on, not necessarily the thread that called
`libsystemd_scan()`/`libsystemd_start_service()`/`libsystemd_stop_service()`.
Like the rest of this module, no internal locking is done - an application
enabling `restart` from a multi-threaded environment is responsible for
serializing its own calls into this module's API.

## Templates

`libsystemd` supports `systemd`-style template units, identified by a literal
`@` in the file name:

- **Template** - `<prefix>@.ini` (empty instance, e.g. `getty@.ini`). Never
  parsed or started on its own; it only exists to be inherited from.
- **Instance** - `<prefix>@<instance>.ini` (e.g. `getty@tty1.ini`). A real
  file must exist for every instance you want started - there is no runtime
  "instantiate on demand" (no `systemctl start foo@bar` equivalent, and the
  underlying `dmvfs` has no symlinks to lean on the way real systemd's
  `.wants/` directories do).

When `libsystemd_parse_dir()` encounters an instance file, it first parses
the matching `<prefix>@.ini` template (if one exists next to it) into the
same `dmini` context, then parses the instance file *on top* - any key the
instance file sets overrides the template's value for that key; every other
key is inherited unchanged. This means an instance file can be empty and
inherit everything, or set just one key (e.g. `args=`) to override only that.
A missing template is not an error - the instance file's own keys are simply
all it has.

Every key's value is then expanded for these specifiers (matched literally,
case-sensitive):

| Specifier | Expands to |
|-----------|------------|
| `%i`, `%I` | The instance name (e.g. `tty1`) |
| `%p` | The template prefix (e.g. `getty`) |
| `%n` | The full instantiated unit name (e.g. `getty@tty1`) |
| `%v` | The caller-supplied `user_value` passed to `libsystemd_start_service()`/`libsystemd_notify_device_added()`, or an empty string if none was given (see [Device rules](#device-rules)) |
| `%%` | A literal `%` |

An unrecognized `%<char>` sequence (or a trailing `%`) is left untouched.
Specifier expansion only happens for instance files - plain (non-`@`) unit
files are never scanned for `%` and can freely contain a literal `%`.

```ini
# getty@.ini - the template
description=Getty on %i
exec=dmgetty
args=--tty %i
```

```ini
# getty@tty1.ini - empty: inherits every key from getty@.ini as-is
```

```ini
# getty@tty2.ini - overrides "args", still inherits "description"/"exec"
args=--tty %i --baud 9600
```

This scans as two units, `getty@tty1` and `getty@tty2` (the bare `getty@.ini`
template contributes nothing on its own) - `after`/`requires`/`start`/`stop`
all address them by their full instantiated name, same as any other unit.
Runnable copies of this example live alongside the other three in
[`app/libsystemd/examples/`](../examples).

### Starting an instance that was never scanned

An instance file is only needed if you want it auto-started at
`libsystemd_scan()` time (the equivalent of an "enabled" unit in real
systemd). `libsystemd_start_service("<prefix>@<instance>")` also works for an
instance that has **no file of its own at all** - if `<prefix>@<instance>` is
not already known, but `<prefix>@.ini` exists in the last-scanned units
directory, it is parsed and specifier-expanded on the fly and the resulting
service is registered exactly as if it had been found during the scan. This
is the closest equivalent this system has to `systemctl start foo@bar` -
there is no `.wants/`-symlink mechanism (`dmvfs` has no symlinks), so this is
the way to bring up an instance that was not written to disk ahead of time:

```
# units/ only has getty@.ini - no getty@tty3.ini anywhere
service start getty@tty3   # resolves getty@.ini, expands %i -> "tty3", starts it
service status getty@tty3  # now shows up, like any other unit
```

Once instantiated this way, the unit stays in the registry (found by
`libsystemd_status()`/`libsystemd_stop_service()`/`libsystemd_list()`, and
not re-instantiated by a second `libsystemd_start_service()` call) until the
next `libsystemd_scan()` replaces the whole registry.

## Dependency ordering

`libsystemd_scan()` resolves each unit's `starting_order` with a
Bellman-Ford-style relaxation: for every unit, look up every name in its
`after`/`requires` lists in the full registry and raise this unit's order to
`(dependency's order) + 1` whenever that is higher than what it already has.
This repeats, one full pass over every unit at a time, until a pass makes no
further changes - or until it has run once per unit in the registry, which
is the point a true DAG is guaranteed to have converged.

A dependency cycle does not hang or crash this - the bounded pass count
simply gets exhausted without fully converging - but it is not currently
detected or logged as an error either; the units involved just end up with
some partially-resolved order.

Once every unit's `starting_order` is resolved, the registry is sorted
ascending by that value (`dmlist_sort`) and every unit is started in that
order via `Dmod_SpawnModule`. A unit that fails to start (e.g. `exec` names
a module that cannot be found) is logged and skipped - it does not stop the
rest of the registry from starting.

## Device rules

`libsystemd_load_rules(rules_dir)` is a separate entry point from
`libsystemd_scan()`/`libsystemd_parse_dir()` - it loads *rules*, not units,
from every `*.ini`/`*.rules` file found anywhere under `rules_dir`,
recursively. A rules file has one or more `[class=<device-class>]` sections,
each with a `start` key:

```ini
# rules/devices.ini
[class=tty]
start=getty@%name

[class=net]
start=dhcpd@%name
```

`libsystemd_notify_device_added(device_class, device_name, user_value)` looks
up the `[class=<device_class>]` section (first match wins if more than one
rules file defines the same class) and substitutes every `%name` in its
`start` value with `device_name`, then calls `libsystemd_start_service()` on
the result - which transparently instantiates it from a template on demand if
needed (see [Starting an instance that was never scanned](#starting-an-instance-that-was-never-scanned)
above), substituting `user_value` for `%v` in that template's own keys.
`user_value` is optional - pass `NULL` if the device has none, in which case
`%v` expands to an empty string. `libsystemd_notify_device_removed(device_class, device_name)`
resolves the exact same target the same way and calls
`libsystemd_stop_service()` on it instead.

```
libsystemd_load_rules("/etc/dmsystem/rules");
libsystemd_notify_device_added("tty", "tty1", "/dev/ttyS1"); // -> starts "getty@tty1", %v -> "/dev/ttyS1"
libsystemd_notify_device_removed("tty", "tty1");             // -> stops "getty@tty1"
```

This is meant to be called by a driver or filesystem module that discovers
devices at runtime (e.g. `dmtty` enumerating serial ports, or `dmdevfs`
noticing a new node under `/dev`) - it reports `(class, name)` plus whatever
extra value it wants forwarded to the template (e.g. the device's path), and
`libsystemd` maps that to a unit via the loaded rules without the driver
needing to know anything about unit names or templates itself.

Note that `%name` here is a distinct, whole-word placeholder handled by the
rules matcher itself, resolved *before* the target unit name is handed to
`libsystemd_start_service()` - it is unrelated to the `%i`/`%I`/`%p`/`%n`/`%v`
specifiers a template's own keys are expanded for once the target is
resolved (those still work as usual inside `getty@.ini` itself). A rule with
no `start` key, or a section not named `class=...`, is ignored. Calling
`libsystemd_load_rules()` again replaces the entire previously loaded rule
set, same "full reload" semantics as `libsystemd_scan()`. A runnable copy of
the example above lives in
[`app/libsystemd/examples/rules/`](../examples/rules).

### Devices reported before rules/units exist yet

A driver module is typically loaded (and starts reporting devices) *before*
`libsystemd_scan()`/`libsystemd_load_rules()` ever run - services are
started only after drivers are up. `libsystemd_notify_device_added()`
accounts for this: every reported `(class, name)` pair (and its `user_value`)
is remembered internally regardless of whether it can be resolved/started
right away, and both `libsystemd_scan()` and `libsystemd_load_rules()` retry
every still-remembered device against the current rules/units state right
after they finish, with the same `user_value` it was first reported with. So
this works regardless of call order:

```
libsystemd_notify_device_added("tty", "tty1", "/dev/ttyS1"); // no rules loaded yet - returns -ENOENT, remembered
libsystemd_load_rules("/etc/dmsystem/rules");                // [class=tty] start=getty@%name - replays it, starts "getty@tty1"
```

as does the reverse (rules loaded first, but the units directory with
`getty@.ini` in it not scanned until later) - either one being the missing
piece is enough to leave a device pending, and either one showing up later
is enough to retry it. A device stops being remembered - and is never
retried again - once `libsystemd_notify_device_removed()` is called for the
same `(class, name)` pair, even if it was never successfully started.
