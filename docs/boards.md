# Supported boards

Reference target: `frdm_mcxn947/mcxn947/cpu0` (NXP FRDM-MCXN947).

| Family | Board targets |
| --- | --- |
| MCXN | `frdm_mcxn947/mcxn947/cpu0`, `frdm_mcxn947/mcxn947/cpu0/qspi`, `frdm_mcxn236`, `mcx_n9xx_evk/mcxn947/cpu0`, `mcx_n9xx_evk/mcxn947/cpu0/qspi`, `mcx_n5xx_evk/mcxn547/cpu0` |
| MCXA | `frdm_mcxa153`, `frdm_mcxa156`, `frdm_mcxa266`, `frdm_mcxa344`, `frdm_mcxa346`, `frdm_mcxa366`, `frdm_mcxa577` |
| KE1xZ | `frdm_ke17z` |
| MCXE | `frdm_mcxe247` |

Which case covers which target is declared in that case's `testcase.yaml` (`platform_allow`), and
that declaration is authoritative — this table is the human summary of it.

- **lpadc, lpcmp, port** cover every MCXN and MCXA target above.
- **lpdac** covers 10 targets: every MCXN one except `frdm_mcxn236`, and every MCXA one except
  `frdm_mcxa153` and `frdm_mcxa344`. Those three SoCs have no `nxp,lpdac` node at all. (`dac2` on
  MCXN947 is an `nxp,hpdac` and a different driver.)
- **vref** covers the MCXN targets only: no MCXA SoC has an `nxp,vref` node.
- **ftm** covers the KE1xZ and MCXE targets only, and nothing else above: no MCXN or MCXA SoC has an
  FTM at all. It is also the one case that does not run all six layers — see
  [../tests/ftm/README.md](../tests/ftm/README.md) for why `dpd` cannot exist on an FTM part and why
  `sysmanaged` runs on both.

The MCXN and MCXA SoCs all `select HAS_PM` and declare the same four power states (`sleep`,
`deepsleep`, `powerdown`, `deeppowerdown`), which is why the layer snippets are family-wide and only
the peripheral wiring is per board.

Per-board wiring lives in `tests/<case>/boards/<board_target>.overlay`, which Zephyr applies
automatically from the board target name — nothing to pass on the command line. A case whose DT
nodes are enabled by every SoC dtsi already needs no such file and has no `boards/` directory;
`port` is the one.

## Not covered

The `cpu1` clusters and the `_ns`/TrustZone board variants. Both would need a different DT address
map and, for `cpu1`, its own board glue. Neither is exercised by the in-tree tests either.

`frdm_mcxa344` builds but has never run on hardware: the only farm station for it is tagged
`@freemaster`. It is recorded `blocked`, which is the honest state.

## Running a layer on a farm station

A station programs a hex, so every build carries one: `lib/pm_test/Kconfig` selects
`CONFIG_BUILD_OUTPUT_HEX`, because a build without one is not a runnable test even though it
compiled.

The capture order that works is flash, attach the console, then reset with it attached:

```sh
dapeng remote start --board frdmmcxn947 --id 1166 --timeout 2h --detach
dapeng remote control flash /c/tf/twister_links/test_0/zephyr/zephyr.hex -y -s <job>
timeout 95 dapeng remote serial -s <job> < /dev/null > baseline.txt &
sleep 8 && dapeng remote control reset -y -s <job>
```

Three ordering rules are worth knowing before a sweep, each of which costs a whole run to rediscover:

- **Attach the console *after* anything that rebuilds a stream.** `control flash` and `control
  powercycle` rebuild the station's streams, and an attached console is told so and then carries
  nothing (`this console is no longer the same conversation`). A reset leaves the streams alone,
  which is why the run is driven by a reset rather than by the flash.
- **The `dpd` layer needs a power cycle, not a reset.** Deep Power Down drops the target's debug
  port, so the flash that follows a `dpd` run reports that `gdb` did not come back and the serial
  stream carries nothing. `control powercycle`, then attach, then reset.
- **`remote serial --forward <port>` cannot be relied on for capture.** Observed on station 1166:
  the stream reported `ready`, the forward held the port, a raw TCP client connected, and zero bytes
  arrived — while `dapeng remote serial` on the same session printed the whole run. The CLI console
  redirected to a file is the capture mechanism; `flash_and_capture.py --port socket://...` silently
  reports every layer as a hang against that forward.

Give the captures to `collect_results.py --console`, which is [results.md](results.md).

## Windows outdir

A short twister `-O` is not cosmetic, and for this repository it is not sufficient either:

```sh
west twister -T zephyr-device-pm-test/tests --build-only -O /c/tw --all --ninja --short-build-path
```

The default outdir sits under the west topdir, and `<outdir>/<board_target>/<toolchain>/<suite
path>/<scenario>/` plus an object path such as
`zephyr/subsys/portability/posix/c_lib_ext/CMakeFiles/subsys__portability__posix__c_lib_ext.dir/getopt_shim.c.obj`
crosses 260 characters for the longer board targets. The compiler then never writes the object,
cmake and ninja say nothing, and the build fails much later in `ar` with `error reading ...obj: No
such file or directory` — which reads like a parallel-build race. It is not: it reproduces at `-j 2`
on an incremental rebuild.

`-O /c/tw` alone still overflows here, because the `<suite path>` component is
`zephyr-device-pm-test/tests/<ip>` and the hal_nxp object paths carry a cmake hash directory. The
symptom shifts rather than disappearing: builds succeed, then the outdir cannot be deleted
(`rm -rf` and `rd /s /q` both report `Permission denied` on the deepest paths, because the tools
cannot name them either), so the next run fails in `shutil.rmtree` with `WinError 5`. Recover with
`cmd /c rd /s /q \\?\C:\tw`, whose `\\?\` prefix lifts the limit. `--short-build-path` avoids the
whole thing by pointing cmake at a symlink; it requires `--ninja`.

## A scattered "Build failure" with no message

A full 13-board sweep is 78 configurations, and twister's default parallelism multiplied by each
build's own `ninja -j` oversubscribes the machine badly enough to produce spurious failures: a
`Build failure` with no compiler diagnostic, typically a generator step such as
`gen_offset_header.py` whose subprocess dies silently, after a `Configuring done (645.5s)` line that
is two orders of magnitude slower than normal.

Read the distribution before reading the code. A real defect lands on every board of a family, or
on every layer of a board. Contention lands on an unrelated scatter — in one observed run, six
failures across five boards and four layers, where every one of those boards built the same layer
successfully in another configuration. Rerunning the six sequentially, one `west build` at a time,
is what distinguishes the two; lowering `-j` on the whole sweep costs more wall-clock than the
retry does.
