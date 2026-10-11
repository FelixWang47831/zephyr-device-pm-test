# CTIMER device PM

Driver under test: `drivers/counter/counter_mcux_ctimer.c`, plus the CTIMER clock gate
in `drivers/clock_control/clock_control_mcux_syscon.c` which the PM callbacks rely on.

Before the change this driver registered a `pm_device` whose `SUSPEND`, `RESUME` and
`TURN_OFF` actions were all empty: PM reported state transitions that had no effect on
the hardware at all. All six layers here are new, so every verdict below is about the
reworked driver.

## What is observable

Two things, because neither alone is enough.

**The AHB clock gate**, read out of the clock controller —
`SYSCON->AHBCLKCTRL1[26..28]` / `AHBCLKCTRL2[21..22]` on MCXN, `MRCC_GLB_CCn` on
MCXA. Never out of a CTIMER register: a CTIMER register only answers while the block
is clocked, and whether it is clocked is the question. Gating that clock is the whole
of what `SUSPEND` does for power — from the counter API a stopped counter and a
suspended one are indistinguishable.

**The register image of an armed alarm** — `MR[0]` and its `MCR` interrupt enable.
This is what tells a restored CTIMER from one that came back from reset, and it has to
be the run-time image rather than the devicetree one: on every board here `prescale`,
`mode` and `input` are all `0`, which is also what a reset leaves.
`src/test_control.c` asserts that the armed image is non-zero.

On top of both, every layer asserts behaviour and not only registers: that the counter
advances, and that the alarm the consumer armed still fires. A CTIMER's functional
clock is selected by the *board*, not by this driver, so a register image that came
back perfectly while the clock mux did not would read as restored and still not count.

## Which action brings the block up

`TURN_ON` rebuilds the register block and `RESUME` restarts the counter, and
`test_device_turn_on_rebuilds_the_register_block` asserts that split rather than
assuming it. `TURN_ON` borrows the function clock to write those registers and gates
it again before returning, which is the part that matters here:
`pm_device_driver_init()` runs `TURN_ON` and then stops short of `RESUME` when runtime
PM is about to own the device, so a driver that left the block clocked would boot it
running while the core records it as `SUSPENDED`. `test_runtime_boots_without_a_consumer`
is the check that it does not happen here.

## The policy constraint

A running counter is stopped by any state that stops its clock, and it has no
completion event that could bound a shorter window. The constraint is therefore held
for as long as the counter is running and the device is active: `counter_start()` and
`counter_stop()` take and release it, a suspend releases it, and a resume takes it
again for a counter that was left running. Without the release on suspend, one
`counter_start()` would block Deep Sleep for the rest of the run even with no consumer
holding a reference. `src/test_system.c` asserts both directions.

Which states to declare depends on the clock the board attached to the counter. Every
SoC dtsi behind the targets in `platform_allow` declares the conservative list, for
every instance; `constraints.overlay` restates it for the `system` layer so that layer
does not depend on those dtsi commits being in the tree. Per MCX Nx4x RM Rev. 6_RC2
Table 336 CTIMER0 is "LP/Static" in Deep Sleep and Power Down, so what stops the
counter there is the clock: every board here attaches a PLL that does not survive
those states.

## Expected per layer

Measured on Dapeng job 2325402, all 11 board targets, at zephyr `33915515a6e` and
harness `81f5137` — [../../results/20261011-ctimer-dapeng.json](../../results/20261011-ctimer-dapeng.json).
66 of 66 configurations built. Six board models had no online station in the run token
pool and are recorded `built`, which is infrastructure and not a verdict.

| Layer | n236 | n947 | a577 | a266 | a153 |
| --- | --- | --- | --- | --- | --- |
| `baseline` | PASS | PASS | blocked | PASS | PASS |
| `device` | PASS | blocked | PASS | blocked | PASS |
| `runtime` | PASS | PASS | PASS | PASS | fail |
| `system` | PASS | PASS | PASS | PASS | fail |
| `sysmanaged` | PASS | PASS | fail | fail | fail |
| `dpd` | PASS | PASS | fail | fail | fail |

No assertion failed anywhere in this job. Every row that is not a PASS is a hang or a
station event.

n236 carries all six layers. `system` passes on every board that reached it, and
`device` and `dpd` pass on every board that reached them.

The remaining rows, none of which is the driver:

`sysmanaged` and `dpd` on the three MCXA boards hang at the forced transition, the log
ending on `forcing one Deep Sleep transition` or `forcing one Deep Power Down
transition`. That is `mcxa-suspend-to-idle-never-wakes`, an environment defect this
case inherits.

`runtime` and `system` on a153 stop after the layer banner, before the suite setup
prints its first line, with every case blocked. Those two layers are the only ones that
boot every device `SUSPENDED` through `PM_DEVICE_RUNTIME_DEFAULT_ENABLE`, and `device`,
which does not, passes on the same board. Not reproduced locally — a153 is not on hand.

`device` on n947 is a station problem, not a board one: the handler log ends on
`PROJECT EXECUTION SUCCESSFUL` and twister then died with `UnicodeEncodeError` printing
its summary tree to a cp1252 console. `baseline` on a577 and `device` on a266 are
download errors — the image never reached the board. A station leaves the previous
task's `handler.log` at the same path, so for a download error that file reports
somebody else's run and must not be read as a verdict.
