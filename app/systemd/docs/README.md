# systemd docs

`systemd` is intentionally thin - it has no public API of its own (nothing
else `dmod_link_modules`s or `target_link_libraries`s against it), so there
is no API reference here. It parses its command line and does exactly one
thing: call `libsystemd_scan()`. All of the actual behavior - the unit file
format, dependency ordering, starting/stopping/status - is documented in
[`libsystemd`'s docs](../../libsystemd/docs).

## Command line

```
systemd <units-directory>
systemd -h | --help
```

Scans `<units-directory>` for `*.ini` unit files, resolves their
`requires`/`after` order, and starts every unit found there in that order
(see [`libsystemd_scan()`](../../libsystemd/docs/api-reference.md#libsystemd_scanconst-char-path)).
Long-running units keep running as independently spawned processes after
`systemd` returns - it does not block or supervise them itself.

```bash
dmod_loader systemd.dmf --args "/etc/dmsystem/units"
```

Exit code is `0` on success, `-EINVAL` if the units directory argument is
missing/malformed, or whatever `libsystemd_scan()` returned on failure (e.g.
`-ENOENT` if the directory does not exist).

See the [repository README](../../../README.md) for the full unit file
format and how `systemd`, `libsystemd` and `service` relate to each other.
