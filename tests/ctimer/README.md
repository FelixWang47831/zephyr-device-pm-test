# CTIMER device PM

Driver under test: `drivers/counter/counter_mcux_ctimer.c`, plus the CTIMER clock gate
in `drivers/clock_control/clock_control_mcux_syscon.c` which the PM callbacks rely on.

Before the change this driver registered a `pm_device` whose `SUSPEND`, `RESUME` and
`TURN_OFF` actions were all empty: PM reported state transitions that had no effect on
the hardware at all. All six layers here are new, so every verdict below is about the
reworked driver.

## What is observable

Two things, because neither alone is enough.

**The AHB clock gate**, read out of the clock controller the way `tests/port` reads it
— `SYSCON->AHBCLKCTRL1[26..28]` / `AHBCLKCTRL2[21..22]` on MCXN, `MRCC_GLB_CCn` on
MCXA. Never out of a CTIMER register: a CTIMER register only answers while the block
is clocked, and whether it is clocked is the question. Gating that clock is the whole
of what `SUSPEND` does for power — from the counter API a stopped counter and a
suspended one are indistinguishable.

**The register image of an armed alarm** — `MR[0]` and its `MCR` interrupt enable.
This is what tells a restored CTIMER from one that came back from reset, and it has to
be the run-time image rather than the devicetree one: on every board here `prescale`,
`mode` and `input` are all `0`, which is also what a reset leaves. A case comparing
the devicetree configuration against reset would compare zero against zero and pass
without the driver restoring anything. `src/test_control.c` asserts that the armed
image is non-zero for exactly that reason.

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
running while the core records it as `SUSPENDED` — the same shape as the
`lpcmp-boots-enabled-while-suspended` finding. `test_runtime_boots_without_a_consumer`
is the check that it does not happen here.

## The policy constraint

A running counter is stopped by any state that stops its clock, and it has no
completion event that could bound a shorter window. The invariant the driver keeps is
that the constraint is held exactly while `TCR[CEN]` is set: `counter_start()` and
`counter_stop()` take and release it, and so do the PM actions that stop and restart
the counter behind the consumer's back. Releasing it on suspend matters — without it,
one `counter_start()` would block Deep Sleep for the rest of the run even with no
consumer holding a reference. `src/test_system.c` asserts both directions.

Which states to declare depends on the clock the board attached to the counter. Every
SoC dtsi behind the targets in `platform_allow` declares the conservative list, for
every instance; `constraints.overlay` restates it for the `system` layer so that layer
does not depend on those dtsi commits being in the tree. Per MCX Nx4x RM Rev. 6_RC2
Table 336 CTIMER0 *can* keep counting in Deep Sleep and Power Down, but only on a
functional clock that survives them, and every board here attaches a PLL that does not.

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
station event, which is what separates this record from the previous one: there the
`system` row was a real defect and the `device` and `dpd` rows were tests asserting a
contract the counter API does not give the driver.

n236 carries all six layers. `system` passes on every board that reached it, which is
what `ctimer-power-lock-held-while-suspended` and
`ctimer-power-lock-leaks-after-turn-off` in
[../../docs/findings.md](../../docs/findings.md) were about: the driver now drops its
policy lock on suspend and keeps the held-ness in its own flag rather than reading a
bit a turn-off resets. `device` and `dpd` pass on every board that reached them, against
the rewritten assertion — a round trip owes a usable device, and a freshly armed alarm
firing is what proves the restore path ran.

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
download errors — the image never reached the board. Note that a station leaves the
previous task's `handler.log` at the same path, so for a download error that file
reports somebody else's run and must not be read as a verdict.
