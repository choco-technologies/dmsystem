# dmsystem API Reference

## Usage

```bash
dmod_loader dmsystem.dmf <path-to-units-directory>
```

## Arguments

| Argument                  | Description |
|----------------------------|-------------|
| `<path-to-units-directory>` | Required. Path to a directory containing one `*.ini` unit file per managed service - see [README.md](../README.md) for the configuration format. |

## Exit codes

- `0` - every unit that was started/run succeeded and there is nothing left
  to supervise (e.g. a directory containing only `oneshot` units).
- Positive value - the number of units that ended up in the `FAILED` state.
- Negative value - the units directory could not even be opened (`-EINVAL` if
  no path was given, `-ENOENT` if the directory could not be opened).
  Individual malformed unit files are logged and skipped, not fatal.

If at least one `simple` (long-running) unit is started successfully,
`dmsystem` does not return: it supervises those units (polling via
[dmosi](https://github.com/choco-technologies/dmosi), respawning any whose
unit declares `restart=always`) until every one of them has terminated.
