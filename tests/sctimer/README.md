# SCTimer device PM

Driver under test: `drivers/pwm/pwm_mcux_sctimer.c`, plus the SCTimer clock gate in
`drivers/clock_control/clock_control_mcux_syscon.c` which the PM callbacks rely on.

Before the change this driver registered a `pm_device` whose `SUSPEND` and `RESUME`
actions did nothing to the hardware, and whose `TURN_ON` re-ran init and left the
block clocked and counting. All six layers here are new, so every verdict below is
about the reworked driver.

## A holding block, not a transactional one

The SCTimer drives its output until the consumer changes it. There is no completion
event a `put()` could hang on, so the driver takes no runtime PM reference of its own
and wrapping each API call in its own get/put would stop the output the moment the
call returned. The reference is the consumer's, and because it is, a `pwm_set_cycles()`
that arrives while nobody holds one is refused with `-EBUSY` rather than writing a
register behind a closed clock gate. `src/test_runtime.c` asserts the refusal and that
the registers did not move.

## What is observable

Three things, because none alone is enough.

**The AHB clock gate**, read out of the clock controller — `SYSCON->AHBCLKCTRL0`, bit
`SCT0`. Never out of an SCT register: an SCT register only answers while the block is
clocked, and whether it is clocked is the question. Gating that clock is the whole of
what `SUSPEND` does for power. The gate constant is taken from the SDK's `SCT_CLOCKS`
macro, the same source the clock driver's own SCTimer cases use, because the enum is
`kCLOCK_Sct` on some parts and `kCLOCK_Sct0` on others.

**The register image of a configured channel** — `MATCHREL[0]` and `MATCHREL[1]`, and
the `OUT[0].SET`/`OUT[0].CLR` event masks. This is what tells a restored SCTimer from
one that came back from reset, and it has to be the run-time image rather than the
devicetree one: the `prescaler` this driver writes is `0` on this board, which is also
what a reset leaves. `src/test_control.c` asserts that the configured image is
non-zero.

**The output, toggling.** Read repeatedly out of `OUTPUT` until both levels have been
seen, because a level read once says nothing — a parked output sits at a level too. An
SCTimer's counter runs off the bus clock, which the board selects and this driver does
not, so a register image that came back perfectly while the clock did not would read
as restored and still drive nothing.

## Telling a replay from a block that was never disturbed

This driver replays the channel the consumer configured from `TURN_ON`, which is what
makes a power domain cycle invisible to the consumer — and also what makes the
register image useless as a power-loss detector: a restored image is what *both* the
`sysmanaged` and the `dpd` layer owe. So `src/test_lowpower.c` writes a witness value
into the last match reload register, which the driver never touches. It survives a
suspend and is gone after a power cycle, and the `dpd` verdict asserts it is gone.
Without it, a block that quietly kept its power across Deep Power Down would pass as a
block that was rebuilt.

In the `device` layer, where nothing really cuts power, the same question is asked by
clobbering the channel registers by hand between `SUSPEND` and `TURN_OFF`.

## Which action brings the block up

`TURN_ON` rebuilds the register block and replays the channel, and `RESUME` restarts
the counter. `test_device_turn_on_replays_the_channel` asserts that split rather than
assuming it. `TURN_ON` borrows the clock to write those registers, parks the block and
gates it again before returning, which is the part that matters here:
`pm_device_driver_init()` runs `TURN_ON` and then stops short of `RESUME` when runtime
PM is about to own the device, so a driver that left the block clocked would boot it
counting — and driving a pin — while the core records it as `SUSPENDED`.
`test_runtime_boots_without_a_consumer` is the check that it does not happen here.

Reading what a gated block holds means borrowing its clock back, which
`sctimer_peek_gated()` does after the gate itself has been sampled. That is also how
the park is observed: `SUSPEND` halts the counter and holds every configured output at
its inactive level while leaving `OUT[n].SET`/`OUT[n].CLR` and the match reloads alone,
so the channel survives and the counter can be restarted without reprogramming it.

## The policy constraint

An output that is driving is stopped by any state that stops the counter's clock, and
it has no completion event that could bound a shorter window. The constraint is
therefore held for as long as a channel is configured and the device is active:
`pwm_set_cycles()` takes it, a suspend releases it, and a resume takes it again for a
channel that was left configured. Without the release on suspend, one
`pwm_set_cycles()` would block Deep Sleep for the rest of the run even with no consumer
holding a reference. `src/test_system.c` asserts both directions.

For the SCTimer the declared list does not depend on the board. Per MCX Nx4x RM
Rev. 6_RC2 section 35.4 Table 336 the SCT is `Static` in Deep Sleep and Power Down —
it has no asynchronous functional clock that could keep it counting there, so no board
overlay could justify dropping `&deepsleep` — and `Off` in Deep Power Down, where the
CORE domain is gated and the registers are reset. `nxp_mcxnx4x_common.dtsi` declares
all three; `constraints.overlay` restates them for the `system` layer so that layer
does not depend on the dtsi commit being in the tree.

## Board coverage

One board target, `frdm_mcxn947`. It is the only one that enables `sc_timer` and
attaches a pinctrl group to it (`pinmux_sctimer`, which routes `SCT0_OUT0` to
`PIO2_2` — one channel, which is why `SCTIMER_PWM_CHANNEL` is `0`). The MCX N EVK
targets never reference `&sc_timer`, so the node stays disabled there and
`src/sctimer.h` stops the build with an `#error` rather than passing vacuously. MCX A
has no SCTimer peripheral at all.

Only `pinctrl-names = "default"` is defined on that node, so the driver's
`pinctrl_apply_state(PINCTRL_STATE_SLEEP)` returns `-ENOENT`, which it tolerates.
Nothing in this case looks at the pad, so no external wiring is needed.

## Expected per layer

Not yet run on hardware. The table goes here once there is a Dapeng job to cite.
