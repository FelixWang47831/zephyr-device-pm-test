# OPAMP device-PM test

Exercises the power-management hooks of `drivers/opamp/opamp_mcux_opamp.c` and
`drivers/opamp/opamp_mcux_opamp_fast.c` across the MCXN and MCXA families, through
the six layers of [../../docs/pm-layers.md](../../docs/pm-layers.md).

## What is observable

The opamp API has no enable or disable call — being `PM_DEVICE_STATE_ACTIVE` is the
only thing that turns the amplifier on — so the register block splits into three
parts instead of LPDAC's two:

| Part | Owner | Checked as |
| --- | --- | --- |
| `EN` (`OPAMP_CTR`/`OPAMP_CTRL`) | `SUSPEND` clears it, `RESUME` puts it back | the amplifier is on / off, 1:1 with the PM state |
| the rest of the register, minus the gain fields on the gain-capable variant | the devicetree configuration, rebuilt by `TURN_ON` | captured after boot, then compared |
| `NGAIN`/`PGAIN` (gain-capable variant only) | runtime state `set_gain()` writes; `RESUME` replays the last value, `TURN_ON` does not touch it | captured after `opamp_set_gain()`, then compared |

The third row is the fix this case exists to prove: a power-gating state resets
`OPAMP_CTR`, and nothing but `RESUME` replaying `data->gain_index` stops the opamp
from coming back enabled at the wrong gain. The fast variant's `set_gain()` returns
`-ENOTSUP` unconditionally and never touches hardware, so it has no third row and no
gain assertions.

The configured value is captured at run time instead of being recomputed from the
devicetree, so the same assertion holds on every part and still fails when a reset
value comes back: the configuration is asserted non-zero in the `baseline` layer,
which is what lets the later layers tell "restored" from "reset".

## Boards

`boards/<board_target>.overlay` is applied automatically from the board target name;
every board dts already enables `&opamp0` and applies its pinctrl state.

| Board target | Variant | Note |
| --- | --- | --- |
| `frdm_mcxn947/mcxn947/cpu0` | gain-capable (`nxp,opamp`) | only board here that completes `sysmanaged`/`dpd` |
| `frdm_mcxn947/mcxn947/cpu0/qspi` | gain-capable (`nxp,opamp`) | XIP variant |
| `frdm_mcxa156` | gain-capable (`nxp,opamp`) | |
| `frdm_mcxa344` | fast variant (`nxp,opamp-fast`) | |
| `frdm_mcxa346` | fast variant (`nxp,opamp-fast`) | |
| `frdm_mcxa366` | fast variant (`nxp,opamp-fast`) | |

Nothing has to be connected to the output pin, and no board here needs a console
move: the opamp pins (`P2_12`/`P2_13`/`P2_15` on the MCXA boards, `PIO4_12`/`PIO4_13`
on MCXN947) do not overlap the console UART on any of them.

Both gain-capable boards' overlays also set `functional-mode = "non_inverting"` and
`non-inverting-reference = "Input1"`. Every in-scope SoC dtsi defaults `opamp0` to
`functional-mode = "follower"`, and `mcux_opamp_apply_gain()`'s `FOLLOWER` case
ignores the gain index entirely — a gain-restore assertion under the default mode
would pass without `set_gain()` having written anything gain-dependent. The
reference override additionally gives `mcux_opamp_configure()` a non-zero `PREF`
value instead of its `UINT8_MAX` default, which the fast-variant boards get for free
since they have no such default-off field.

The three fast-variant boards' overlays set `bias-current = <1>` and
`compensation-capcitor = <1>`: both default to 0, so `mcux_opamp_fast_configure()`
would otherwise write an all-zero configuration region, and `test_device.c`'s
"the clobber actually changed something" check needs a non-zero value to clobber.

Excluded, with a reason: `lpcxpresso55s36` wires an `nxp,opamp` but its SoC dtsi
(LPC55S3x) has no power-management fix yet — a RM-driven follow-up, not a gap in
this case.

## What each layer asserts

| Layer | Assertion |
| --- | --- |
| `baseline` | `EN` is set once ACTIVE, the configuration is non-zero, and (gain-capable only) `set_gain()` succeeds and writes non-zero gain fields — the control, compiled into every layer |
| `device` | boots ACTIVE; `SUSPEND` clears `EN` and leaves the configuration and gain alone; `set_gain()` is then refused with `-EBUSY` without changing the stored gain; `RESUME` restores `EN`, the configuration, and the gain; a second `RESUME` is `-EALREADY`. A second test clobbers the register by hand and requires `TURN_ON` to rebuild the configuration while leaving `EN` and the gain fields at 0 — restoring those is `RESUME`'s job, not `TURN_ON`'s |
| `runtime` | boots with no consumer, so `EN` is clear and (gain-capable only) `set_gain()` is `-EBUSY`; a reference brings the amplifier up, a nested get/put keeps it up, the last put takes it down, and the next get brings it back with the same gain and no second `set_gain()` |
| `system` | the driver's declared disabling states match what the application locks around a held reference, and the amplifier is usable (and, gain-capable, settable) under that lock |
| `sysmanaged` | configuration, `EN`, and gain survive one forced Deep Sleep |
| `dpd` | the same across Deep Power Down, where `TURN_ON` really does rebuild the configuration from reset — this is the layer that proves the `RESUME` gain-replay fix against a real power cycle, not a synthetic one |

Neither driver variant takes a runtime PM reference or a policy lock of its own:
being ACTIVE is the amplifier's only on/off switch, so the `system` layer's lock and
reference are both taken by the test the way an application has to take them,
following `tests/lpcmp`'s pattern rather than `tests/lpdac`'s.
`constraints.overlay` restates the `zephyr,disabling-power-states` list the SoC
dtsi already gives `&opamp0`, so the `system` layer does not depend on that half of
the work.

In the `runtime` layer the device may boot `OFF` rather than `SUSPENDED` — a child
of a `power-domain-soc-state-change` domain never gets `TURN_ON` there; see
`pd-soc-state-change-no-turn-on-under-runtime-pm` in
[../../docs/findings.md](../../docs/findings.md). It costs this driver nothing: the
configuration and the gain are both written by calls that need a reference anyway.
Both states are accepted.

On MCXA the `sysmanaged` and `dpd` layers do not return from the transition for
reasons outside this driver (`mcxa-suspend-to-idle-never-wakes`, same document);
`frdm_mcxn947` is the board that actually completes them.
