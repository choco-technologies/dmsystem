# service docs

`service` is intentionally thin - it has no public API of its own, so there
is no API reference here. Every subcommand is a thin wrapper over one
`libsystemd` call; see [`libsystemd`'s docs](../../libsystemd/docs) for what
each one actually does and its exact return codes.

## Command line

```
service list
service status [unit_name]
service start   <unit_name>
service stop    <unit_name>
service restart <unit_name>
```

| Subcommand | Calls | Behavior |
|------------|-------|----------|
| `list` | `libsystemd_list()` | Prints every unit currently in the registry (name, state, pid). Prints "No units are currently registered." if empty. |
| `status` | `libsystemd_list()` or `libsystemd_status()` | With no unit name, behaves like `list`. With one, prints just that unit's state/pid. |
| `start <unit>` | `libsystemd_start_service()` | Starts one unit. |
| `stop <unit>` | `libsystemd_stop_service()` | Stops one unit. |
| `restart <unit>` | `libsystemd_stop_service()` then `libsystemd_start_service()` | Stops (tolerating "wasn't running") then starts. |

`start`/`stop`/`restart` act only on the named unit - they do not cascade to
its dependencies.

```bash
dmod_loader service.dmf --args "status webserver"
```

**Important caveat:** `service` reaches the exact same in-memory unit
registry a `systemd` in the *same process* populated - see the
["Modules in this repository"](../../../README.md#modules-in-this-repository)
section of the repository README. Run as a separate `dmod_loader` process
from `systemd`, it will simply report zero units, since its own `libsystemd`
instance was never scanned.
