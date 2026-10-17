# NGX lifetime and frame pacing adaptations

Focused adaptations from DarkIzuku/bloodborne_pc `public-stability-dlss-v1`,
reviewed on 2026-10-16:

- `dceac3f4789c0125bc7df446c56b135590c8d146`: owned NGX initialization strings,
  feature retirement at frame boundaries, submitted-work waits that keep the
  current command buffer valid, fresh parameters and temporal history reset.
  Upstream credits AwesomeObserver/windows for its NGX integration work.
- `4afaeece57f76ae3ca2882ab3eb5a804a2c9a735`,
  `998b75cbeaf139a63da095c13a48429fd17e34ee`,
  `49421402b128bbde702831be0171a84eda185c38` and
  `58ee8f5451f0caac7e7b6bb0e5aee123218b1d8b`: frame pacing, read-only verified title/loading state and
  timer/vblank phase corrections. New frame-pacing sources retain their
  GPL-3.0-or-later notices.
- `39dc4a4fa311479d9ee39b2cff7f0b4061f5ff20`: uncapped gameplay timing adaptation based on Kyo's FPS++ job
  design and Awakenchan/BloodBorneButPc `4f93643e`. Measured physics/AI steps
  originate from droogie/bbhost `566184a7`, `src/engine/frame_rate.cpp`.
  The ELF byte lookup helper comes from the same upstream branch's
  `scripts/render_pools.py` in `aa2b89143d2378e445f2253105f4cdb0a9f47a31`; the render-pool patch is not applied.

Existing shadPS4/NGX implementation notices remain GPL-2.0-or-later. The new
timing adaptation retains GPL-3.0-or-later. License texts are included here.
No NVIDIA SDK source or headers are copied into this repository. The pinned
DLSS runtime and its existing distribution terms remain unchanged.

Local changes add partial-handle guarding, core library unloading, failure-path
tests, real Vulkan/NGX checks, explicit user VSync choice and an uncapped-only
launcher option. The upstream policy forcing FIFO for high FPS is not included.
