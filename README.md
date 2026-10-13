# zephyr-device-pm-test

Out-of-tree test harness for **Zephyr device power management** on NXP MCUs.

Many NXP drivers in Zephyr either do not implement device PM, or implement it but have never been
exercised, so it is unknown whether the PM hooks work. This harness builds one focused test per
driver, drives it through six increasingly demanding PM layers, and reports per layer whether each
transition behaves. A failing layer is a finding, not a broken harness.

| Case | IP | Driver under test |
| --- | --- | --- |
| [tests/ctimer](tests/ctimer) | CTIMER | `drivers/counter/counter_mcux_ctimer.c` |
| [tests/ftm](tests/ftm) | FTM | `drivers/pwm/pwm_mcux_ftm.c` |
| [tests/lpadc](tests/lpadc) | LPADC | `drivers/adc/adc_mcux_lpadc.c` |
| [tests/lpcmp](tests/lpcmp) | LPCMP | `drivers/comparator/comparator_nxp_lpcmp.c` |
| [tests/lpdac](tests/lpdac) | LPDAC | `drivers/dac/dac_mcux_lpdac.c` |
| [tests/port](tests/port) | PORT pin-mux | `drivers/pinctrl/pinctrl_nxp_port.c` |
| [tests/sctimer](tests/sctimer) | SCTimer | `drivers/pwm/pwm_mcux_sctimer.c` |
| [tests/vref](tests/vref) | VREF | `drivers/regulator/regulator_nxp_vref.c` |

What it has found so far: [docs/findings.md](docs/findings.md).

## Prerequisites

A **freestanding Zephyr application and Zephyr module**. It has no `west.yml` of its own and does not
vendor Zephyr; it builds against the Zephyr already in your west workspace, and each test pulls this
repository in as a module itself, so `west build` needs no extra flag.

- Zephyr **v4.4.x** (developed against `4.4.99`).
- The NXP HAL module (`hal_nxp`) from the Zephyr manifest.
- A working Zephyr build environment (`west`, Zephyr SDK).

Clone it *inside* your west workspace, next to `zephyr/`:

```
zephyrproject/
  zephyr/
  zephyr-device-pm-test/   <-- here
```

`west twister` needs one thing that `west build` does not. Twister decides whether a scenario's
`required_snippets` exist *before* it configures anything, so it never sees the module this
repository's `CMakeLists.txt` adds, and every layer scenario would be filtered out as
`Snippet pm-* not found`. Point it at the repository once per shell:

```sh
export ZEPHYR_EXTRA_MODULES=/c/repos/workspace/zephyrproject/zephyr-device-pm-test
```

## Running

```sh
BOARD=frdm_mcxn947/mcxn947/cpu0

# One layer, one board.
west build -b $BOARD zephyr-device-pm-test/tests/lpadc -p always -S pm-runtime
west flash

# Every layer, every board the cases declare.
west twister -T zephyr-device-pm-test/tests -O /c/tw --all --ninja --short-build-path
```

`-S <snippet>` selects the layer and is the whole interface; omit it for `baseline`. The one exception
is `system`, whose state list is per peripheral and so comes from the case's own overlay
(`-- -DEXTRA_DTC_OVERLAY_FILE=constraints.overlay`); twister does this for you. The layers, and what
each one proves, are in [docs/pm-layers.md](docs/pm-layers.md). On Windows both a short `-O` and
`--short-build-path` are required — [docs/boards.md](docs/boards.md#windows-outdir) says why.

Tests are ztest suites, so twister reports per check rather than per image, and "not observable on
this SoC" is a skip rather than a silent pass.

## Layout

```
tests/<ip>/          one case per driver: layers as files, wiring as overlays
snippets/pm-*/       the layer definitions - the only place a layer is configured
lib/pm_test/         forced low-power transition, console revival, layer assertions
include/pm_test/
docs/                see below
results/             what has actually run, generated from twister output
scripts/             collect_results.py
```

## Documentation

| | |
| --- | --- |
| [docs/pm-layers.md](docs/pm-layers.md) | **normative**: the six layers, what each proves, how to run one |
| [docs/findings.md](docs/findings.md) | every defect found, with status — one read for "what is still open" |
| [docs/boards.md](docs/boards.md) | supported board targets, what is out of scope, the Windows outdir trap |
| [docs/authoring-a-case.md](docs/authoring-a-case.md) | how to add a driver case |
| [docs/results.md](docs/results.md) | the run-record schema and how to produce one |
| [docs/zephyr-pm-notes.md](docs/zephyr-pm-notes.md) | facts about Zephyr's PM subsystem that cost us a layer each to learn |

## Why the results are in the repository

The [Device PM report](https://zhaoxiangjin.github.io/zephyr-data/reports/device-pm/) derives device
PM *enablement* by statically analysing the Zephyr tree, but no amount of source analysis can say
whether a transition was ever exercised. That evidence only exists here, so it is recorded in a form
a tool can read: `case.yaml` ties a case to the compatibles it covers, `testcase.yaml` declares the
board targets and the layers, and `results/*.json` records the runs.
