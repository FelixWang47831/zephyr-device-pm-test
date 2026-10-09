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

Which states to declare depends on the clock the board attached to the counter. The
MCX Nx4x SoC dtsi declares the conservative list for all five of its instances;
`constraints.overlay` supplies it on the MCXN23x and MCXA targets, whose SoC dtsi does
not. Per MCX Nx4x RM Rev. 6_RC2 Table 336 CTIMER0 *can* keep counting in Deep Sleep
and Power Down, but only on a functional clock that survives them, and every board
here attaches a PLL that does not.

## Expected per layer

Measured on `frdm_mcxn947/mcxn947/cpu0`, station 1166, against an earlier shape of the
driver that configured the block from `RESUME` rather than `TURN_ON`, and before the
SoC dtsi put the CTIMERs on the CORE domain. The current driver is build-verified
only, so every row below has to be re-run.

| Layer | Verdict (stale) |
| --- | --- |
| `baseline` | PASS — the counter counts and an armed alarm fires |
| `device` | PASS — `SUSPEND` gates the clock, the round trip restores the alarm |
| `runtime` | blocked — see below |
| `system` | PASS — the constraint tracks `TCR[CEN]`, including across a runtime suspend |
| `sysmanaged` | PASS — the sweep's `SUSPEND`/`RESUME` round trip |
| `dpd` | PASS — the armed alarm comes back from a real Deep Power Down |

`dpd` was the layer whose outcome could not be predicted from the source. The CTIMER
functional clock select and divider (`CTIMER0CLKSEL`, `CTIMER0CLKDIV`) live in SYSCON
and are programmed by board init, not by this driver, so a register image that came
back perfectly while the clock mux did not would have read as restored and still not
counted. It does count: after the transition the gate is re-opened, `MR[0]` and `MCR`
are bit-identical, and the alarm the consumer armed fires.

`runtime` never reaches its own claims on this board, and not because of this driver.
The MCXN SoC dtsi enables a `core-domain` node of compatible
`power-domain-soc-state-change`, and that driver's `pd_pm_action()` dereferences
`pm_state_next_get()` unconditionally. In a build with `CONFIG_PM_DEVICE_RUNTIME` but
no `CONFIG_PM` — which is exactly this layer — that helper is a stub returning `NULL`,
so `pm_device_runtime_auto_enable()` faults during device init, before the console
UART is initialised: the board produces no output at all and the fault message has no
backend to reach. `pd-soc-state-change-null-deref` in
[../../docs/findings.md](../../docs/findings.md).

The layer's central claim is still observable, in `system`, which selects `CONFIG_PM`
as well and so gets past that fault: `ctimer@c000 at boot: suspended` with `gate=0`.
That is the whole point of `TURN_ON` handing the function clock back before it
returns.
