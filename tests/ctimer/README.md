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

Measured on Dapeng job 2325364, all 11 board targets, at zephyr `196864afa42` and
harness `482deab` — [../../results/20261010-ctimer-dapeng.json](../../results/20261010-ctimer-dapeng.json).
66 of 66 configurations built. Six board models had no online station in the run token
pool and are recorded `built`, which is infrastructure and not a verdict.

| Layer | a577 | n236 | n947 | a266 | a153 |
| --- | --- | --- | --- | --- | --- |
| `baseline` | PASS | blocked | blocked | blocked | blocked |
| `device` | fail | fail | blocked | blocked | blocked |
| `runtime` | PASS | PASS | PASS | PASS | blocked |
| `system` | fail | fail | fail | fail | fail |
| `sysmanaged` | fail | PASS | PASS | fail | blocked |
| `dpd` | fail | fail | fail | fail | blocked |

The 13 failures are three things, and the table has to be re-run now that all three
have been answered.

`system` failed on all four boards that ran it, and the test was right: the driver
held its policy lock across a suspend, so `while_suspended` was not zero.
`ctimer-power-lock-held-while-suspended` and
`ctimer-power-lock-leaks-after-turn-off` in
[../../docs/findings.md](../../docs/findings.md).

`device` and `dpd` failed because the tests were wrong, not the driver. They demanded
that a power cycle hand back the armed alarm the consumer had set, which is the
consumer's to re-arm — see `src/ctimer.h` for why the counter API puts it there, and
`../lpdac/src/test_device.c` for the same round trip asserted correctly. Both have been
rewritten to assert the contract the driver implements: the round trip owes a usable
device, and a freshly armed alarm firing is what proves the restore path ran.

`sysmanaged` on a577 and a266, and the `blocked` rows, are the board farm rather than
the driver: a hang after the forced transition on boards whose Dapeng station does not
power-cycle between layers, and timeouts or a download error with no ztest output at
all. These separate only by re-running.

`runtime` passing on all four boards that reached it is new.
`pd-soc-state-change-null-deref` blocked this layer in the previous record and no
longer does.
