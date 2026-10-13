# FTM device PM

Driver under test: `drivers/pwm/pwm_mcux_ftm.c`.

The FTM is brought up from `RESUME` rather than from `TURN_ON`. `TURN_ON` never
arrives at run time -- no FTM node is on a power domain -- so a bring-up placed there
would be dead code. `pm_device_driver_init()` also stops short of `RESUME` when
runtime PM is about to own the device, which is what lets an instance boot SUSPENDED
with its clock genuinely gated instead of running behind a PM state that says
otherwise. `src/test_runtime.c` is the check.

## Five layers, not six

There is no `dpd` scenario. No SoC with an FTM declares a suspend-to-ram power
state -- `ke1xz` and `mcxe24x` both stop at `suspend-to-idle`, offered as the three
PSTOPO substates `stop`, `pstop1` and `pstop2` -- so `pm-dpd` cannot even be
configured: its overlay enables `&deeppowerdown`, which is not a label that exists
on these parts.

What `dpd` would have proved is carried by `sysmanaged` instead. `SUSPEND` calls
`FTM_Deinit()`, which halts the counter and then gates the peripheral clock, and
`RESUME` brings it up from scratch with `FTM_Init()` before replaying the
channel setup from RAM. The register image is therefore unreachable for the whole of
any transition, whether or not the silicon cut power, so a driver with no restore path
fails in `sysmanaged`.

`src/test_device.c` also clobbers the registers by hand between `SUSPEND` and
`TURN_OFF`, so the replay is exercised from an arbitrarily corrupted image rather than
from one that merely survived a clock gate.

## What is observable

Three things.

**The peripheral clock gate**, read out of the PCC. `kCLOCK_Ftm<n>` is a public HAL
enum whose value is the absolute address of the PCC register holding that instance
gate, so the gate bit is `PCC_CLKCFG_CGC_MASK` in the word at that address. Never
read out of an FTM register: an FTM register only answers while the peripheral is
clocked, and whether it is clocked is the question. Gating that clock is the whole
of what `SUSPEND` does for power.

The instance number comes from `FTM_BASE_PTRS` rather than from the devicetree
`clocks` cell. The first `clocks` entry of an FTM node names the counter clock
*source*, not the gate: `frdm_ke17z` points it at SCG and `frdm_mcxe247` at PCC, so
no single cell can name the gate for both. It is also why the driver gates through
`FTM_Deinit()` instead of `clock_control_off()` -- `mcux_scg_off()` is `return 0`, so
the call would report success and change nothing.

**The register image of a running PWM** -- `MOD`, `CONTROLS[0].CnV` and `SC`. This
is what tells a restored FTM from one that came back from reset. Both `MOD` and
`CnV` read zero out of reset and non-zero once a consumer has asked for a waveform.
`src/test_control.c` asserts that the running image is non-zero.

**`CNT` advancing**, as the control. An FTM takes its counter clock through a source
and a prescaler that the board devicetree picks rather than this driver, so a
register image that came back perfectly while the clock source did not would read
as restored and still not count.

## The policy constraint

A running PWM is stopped by any state that stops its counter clock, and it has no
completion event that could bound a shorter window. The invariant the driver keeps is
that the constraint is held exactly while a channel is driving an output:
`pwm_set_cycles()` takes it, the PM actions that halt and restart the counter behind
the consumer's back hand it back and take it again, and a `pwm_set_cycles()` with a
period of 0 hands the channel back and drops it. Releasing it on suspend matters --
without it, one `pwm_set_cycles()` would block those states for the rest of the run
even with no consumer left holding a reference. `src/test_system.c` asserts all three
directions.

The zero-period release is a driver extension, not something the PWM API defines. It
has to be the period: the API already gives pulse 0 a meaning -- a constant inactive
level, which is still a waveform the FTM generates -- so 0/0 is the only combination
left to mean "drive nothing". `pwm_mchp_xec.c` reads 0/0 the same way.
`ftm_release_pwm()` in `src/ftm.c` is what the test calls to reach it.

All three PSTOPO options stop the counter. KE1XZ RM Rev. 2 Table 21-4 marks the FTM
`static` in Stop -- registers retained, not clocked -- with none of the `FF in PSTOP2`
exceptions that CRC and LPIT carry on the same page, and the FTM is absent from
Table 21-5, the AWIC Stop wake-up sources, which lists every module that table marks
`Async operation`. MCXE24x RM Rev. 2 Table 34-4 marks it `OFF` in both STOP1 and
STOP2, which Table 34-3 there defines as module and clock sources disabled. So
`frdm_ke17z` pointing `ftm2` at SOSC through SCG, where the SoC default is the FIRC
async clock through PCC, changes nothing.

That reading is by hand. `src/test_system.c` asserts that the driver's locks match
what the node declares, never that a declared state really halts the counter, so no
layer here would catch an over-declared state. It also leans on the node carrying a
list at all, and the SoC dtsi is the only thing supplying one: with no list
`declared_states()` returns 0 and both tests print a line and skip, which is a visible
non-verdict rather than a failure.

`src/test_system.c` iterates substates rather than asking about substate 0 alone.
Both SoCs here offer `suspend-to-idle` as three substates, so asking only about
substate 0 would miss the locks on the other two.

## Board targets

| Target | SoC | Instance | Notes |
| --- | --- | --- | --- |
| `frdm_ke17z` | ke1xz | `ftm2`, channel 0 | Board devicetree already configures it as a PWM for the RGB LED, so there is no `boards/` file. Channel 0 is PTD10, the red LED, so a running waveform is visible. |
| `frdm_mcxe247` | mcxe24x | `ftm0`, channel 0 | The board devicetree leaves `ftm0` as a plain `nxp,ftm`, so `boards/frdm_mcxe247.overlay` promotes it, with the pin and clock setup `tests/drivers/pwm/pwm_api` already uses there. `boards/frdm_mcxe247.conf` also raises `CONFIG_IDLE_STACK_SIZE` -- see below. |

`boards/frdm_mcxe247.conf` exists for a board defect, not for anything the case wants.
This board leaves the idle thread 320 bytes, and the whole `pm_suspend_devices()` /
`pm_resume_devices()` sweep runs on it with immediate-mode log formatting alongside, so
`sysmanaged` faults with a stack overflow the instant the forced Deep Sleep returns.
Raising it to 1024 lets the layer reach its claims about the FTM instead; nothing it
asserts changes. The defect is `mcxe247-idle-stack-too-small-for-pm-sweep` in
[../../docs/findings.md](../../docs/findings.md) and is still open -- any device-PM
sweep on this board is exposed, and a build with the zero-period release path removed
faults identically.

The case covers whatever `nxp,ftm-pwm` instances the board target enables, so a
board that enables more than one is covered without a per-board file.

Both targets run `sysmanaged`, and both need a low-power companion counter to end the
forced Deep Sleep. `frdm_ke17z` has one in its own devicetree: it chooses
`zephyr,system-timer-companion = &lptmr0`, and the ke1xz `lptmr0` is enabled and
carries `wakeup-source`. The MCXE24x `lptmr0` is disabled and not marked wake-up
capable, so `boards/frdm_mcxe247.overlay` enables it, marks it and chooses it.

## Expected per layer

Verified on hardware on both targets: `frdm_ke17z` (FTM2, channel 0) and
`frdm_mcxe247` (FTM0, channel 0). All five layers pass on both, 12 of 12 test cases
per target --
[../../results/20261013-ftm-local.json](../../results/20261013-ftm-local.json).

| Layer | Verdict |
| --- | --- |
| `baseline` | PASS — `pwm_set_cycles()` is accepted, `CNT` advances, `MOD`/`CnV` are non-zero |
| `device` | PASS — `SUSPEND` gates the clock, and `RESUME` rebuilds the waveform from a register image that was clobbered by hand while `TURN_ON` left the gate shut |
| `runtime` | PASS — boots SUSPENDED with the gate closed, and the waveform comes back with the reference rather than having to be asked for again |
| `system` | PASS — the constraint appears with the waveform, goes away on a zero-period release and again on a runtime suspend, and is taken back on resume, across all three PSTOPO substates |
| `sysmanaged` | PASS — the sweep `SUSPEND`/`RESUME` rebuilds the peripheral across a real forced Deep Sleep |

One of those carries a caveat that is not about the FTM.

`frdm_mcxe247` reaches its `sysmanaged` claims only with `boards/frdm_mcxe247.conf`
raising the idle stack; without it the layer faults before asserting anything. See
`mcxe247-idle-stack-too-small-for-pm-sweep`.

That entry is in [../../docs/findings.md](../../docs/findings.md).

## Flashing and capturing

`frdm_ke17z` carries CMSIS-DAP firmware on its onboard debugger, so the default
linkserver runner is the one that works; `west flash --runner jlink` answers `Cannot
connect to the probe/programmer`. Its console enumerates as a plain USB serial device
and pyserial opens it. linkserver leaves the core running the old image when it was
not halted first, so a reflash alone can silently re-run the previous layer -- reset
explicitly (`pyocd reset -t ke17z7`) with the port already open.

`frdm_mcxe247` flashes through the jlink runner. Its console is a J-Link CDC UART,
which re-enumerates around a reset, and that costs two things:

- A twister `--device-testing` sweep opens the port once, so it can die with
  `FileNotFoundError` on every scenario before flashing anything. Retry the run; do
  not replug.
- Reading the port by hand is less reliable than twister's own pyserial handler, which
  does work here. pyserial has been seen to fail on this port with `exception: stack
  overflow` out of `SetCommState` rather than a permission error; a .NET
  `System.IO.Ports.SerialPort` reader opens it without complaint and is what the
  manual captures use.

## Running

```sh
BOARD=frdm_ke17z

west build -b $BOARD zephyr-device-pm-test/tests/ftm -p always            # baseline
west build -b $BOARD zephyr-device-pm-test/tests/ftm -p always -S pm-device
west build -b $BOARD zephyr-device-pm-test/tests/ftm -p always -S pm-system
```

Naming the layer is the whole command line; the `baseline` layer has no snippet and
needs nothing at all.
