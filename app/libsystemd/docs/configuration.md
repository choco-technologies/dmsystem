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

Only files directly inside the scanned directory and ending in `.ini` are
considered; subdirectories and everything else are silently skipped.

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

A unit file with no `exec` key fails to parse (`libsystemd_parse_file()`
returns `-EINVAL`); `libsystemd_parse_dir()` logs that failure and skips the
file rather than aborting the whole scan.

`description`, `type` and `restart` are documented in the
[repository README](../../../README.md#configuration-format) as
**not yet implemented** - they are reserved for a future supervise-loop /
richer scheduling pass, but today's parser never reads them.

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
