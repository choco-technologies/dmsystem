# Library lifecycle validation — 2026-10-16

- Native libsystemd diagnostic: 63/63, including cold loading a DIF-only
  library, a module already enabled through an API dependency, two unit
  instances sharing one module, failed starts, status, removal/re-add and
  refreshed %v paths. The additional plain-library test performs two full load/enable,
  duplicate-START, disable/unload and restart cycles without lifecycle DIF.
- Native systemd and service command diagnostics: 1/1 each.
- Full Cortex-M7 build passed, including both lifecycle helper modules.
- STM32F746G-DISCO integration with dmdevfs and dmutc: the RTC event starts
  dmutc@rtc automatically after flash/reset with type=library and pid=0.
  dmutc diagnostic 6/6, RTC 12/12 and RNG 6/6. The UTC test stops/restarts a
  library unit without unloading API consumers and removes/re-adds the RTC
  with both an invalid new path and the actual path.

The cold-load test is separate from the API-imported helper: DIF lookup must
happen after enabling the library. The changed-path test ensures a retained
inactive template instance expands the new event value instead of keeping
an old device path.

Board console evidence is maintained in the dmutc PR alongside its diagnostic.

## Complete boot integration regression

The dmod-boot CI Renode runner passed console/VFS, SD identification and
automount, read/write across unmount/remount, LCD splash/selftest/fill/bars/
gradient and touch coordinates/watch tests. A second run with the board audio
models enabled also passed those checks. The host lacks root and /dev/net/tun,
so emulated network testing was skipped explicitly; physical board DHCP,
board-to-router ping, host-to-board ping and telnet command execution passed.
The physical board also rejected stopping the plain ICMP library while dmudp
requires it, preserving the running service and ping responses.

The boot integration results are summarized in
https://github.com/choco-technologies/dmod-boot/pull/132.
Raw board and emulator logs are retained locally rather than published.

With the emulator audio models enabled, a one-second 523 Hz tone plays with
zero underruns. Captured stereo PCM has equal channels and a measured frequency
of about 522 Hz by zero crossings, consistent with the requested tone and the
measurement window. This checks signal generation, not clock calibration.

Network qualification: fresh host-to-board ping sessions lose their first
request (extended probe: 9/10), while the gateway control probe is 3/3. The
cause remains unresolved; the board network checks do not establish loss-free
operation. The skipped Renode TAP test also remains a validation limitation.
