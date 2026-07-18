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

A unit file with no `exec` key fails to parse (`libsystemd_parse_file()`
returns `-EINVAL`); `libsystemd_parse_dir()` logs that failure and skips the
file rather than aborting the whole scan.

`description`, `type`, `restart` and `stdlog` are documented in the
[repository README](../../../README.md#configuration-format) as
**not yet implemented** - they are reserved for a future supervise-loop /
richer scheduling pass, but today's parser never reads them.

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
