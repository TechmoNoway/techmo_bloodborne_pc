# NGX lifetime and frame pacing

This batch changes DLSS resource lifetime and Windows presentation timing. Source
identities and license notices are in [the port notice](../../licenses/NGX-PACING-PORTS.md).
The installed DLSS 310.9.1 model is retained.

Local validation on 2026-10-16 passed: 157 Python tests, 537 WPF assertions,
the pacing/state/timer fixture, 16 independent FPS/presentation combinations,
the fake-driver lifecycle fixture, real NGX recreation/evaluation and native
runtime/Vulkan smoke. Real NGX ran on an RTX 4060 Laptop GPU with eight feature
configurations and 16 evaluations, followed by successful shutdown. Gameplay
acceptance, Unlimited physics/cutscenes and five-hour sessions remain pending.

## NGX ownership

- Initialization paths and common-info storage remain owned until NGX shutdown.
- A settings/provider change retires the old feature at the next frame boundary,
  before a pass acquires its command buffer. GPU work completes first.
- An unexpected geometry/format/quality change inside the current submission
  defers recreation to the boundary. Earlier submitted work can be waited on
  without closing the pass's current command buffer.
- Failed creation with a partial handle retains that handle until safe retirement.
  A second call cannot evaluate the partial feature, even if its key matches an
  older successful feature. Parameters are recreated cleanly and history resets
  on successful feature creation.
- Shutdown releases features and parameters after GPU completion, calls the
  driver's no-argument Vulkan shutdown wrapper, then unloads the core reference.
  The driver export signatures differ from the static SDK wrapper; the installed
  core's shutdown exports were inspected before this adaptation.

## User presentation choice

The launcher offers 30, 60, 90 and Unlimited, with a VSync checkbox. Checked selects
FIFO; unchecked restores the previous Mailbox/Immediate choice. The advanced
presentation selector remains usable; FIFO Relaxed also reflects an enabled
VSync checkbox. FPS changes do not force FIFO. Saved choices round-trip, and a
launch explicitly overrides any inherited presentation setting with that choice.

With FIFO/FIFO Relaxed, software production respects the actual display refresh,
including fractional rates and display/mode changes. Without VSync, Unlimited
has no gameplay software cap. The surface's supported-mode fallback still applies.

Title/loading has a maximum of 60 FPS, independently of optional PC controls or
native menu assets. A read-only predicate checks the supported 1.09 identity and
original bytes; unknown images/states keep the safe ceiling. Inventory/shop/System
pages in an active world remain gameplay. The limiter retains deadline phase
across ordinary timer delays and rephases after longer stalls without catch-up
bursts. The vblank event loop also keeps its phase and returns to publish events
even when its flip queue never empties.

Unlimited uses a byte-verified adaptation of the FPS++ frame-job design and measured
physics/AI steps, rather than the retired legacy uncapped job patch. Only the
prepared executable receives those writes. Selected external patches still win,
and their final ranges are passed into red-zone preparation before detours are
created. No 120 FPS launcher option is introduced. Unlimited remains experimental:
long gameplay, combat, boss introductions and in-engine cutscenes need acceptance.
Encoded video timestamps and source frame rates are unchanged.

## Verification

```sh
ninja -C out/gpu -j2 frame-pacing-test ngx-lifetime-test
out/gpu/frame-pacing-test.exe
out/gpu/ngx-lifetime-test.exe --real
```

The pacing fixture replays timer overshoot, stalls, state transitions, fractional
refresh, inventory exclusion and a vblank-driven producer. Runtime presentation
choices are tested in separate processes to cover cached environment settings.
Python tests check uncapped timing writes, byte guards and user VSync preservation.
The WPF verifier uses synthetic launch/config/game fixtures outside the live root.

The NGX fixture compiles the production implementation with a fake driver boundary
on a real Vulkan scheduler, covering partial creation and allocation failure,
safe retirement, parameter cleanup and command-buffer validity. Its real-driver
mode checks repeated feature creation/evaluation across quality, DLAA and output
size changes and reads back finite, nonzero GPU output. Hosted CI compiles this
fixture; it runs locally on a compatible RTX GPU.

Automated checks do not establish a gameplay FPS gain, input latency improvement
or stability over a five-hour session. Test the existing 60/90 modes first, then
compare Unlimited with VSync enabled/disabled on the tester's actual display.
