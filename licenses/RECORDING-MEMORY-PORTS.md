# Deferred recording and runtime mapping fixes

Focused adaptations from DarkIzuku/bloodborne_pc, branch
`public-stability-dlss-v1`, reviewed on 2026-10-16:

- [dceac3f4789c0125bc7df446c56b135590c8d146](https://github.com/DarkIzuku/bloodborne_pc/commit/dceac3f4789c0125bc7df446c56b135590c8d146):
  `gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h`,
  `tests/test_record_data.cpp` and `tests/test_memory_notifications.c`.
  The upstream recording implementation credits AwesomeObserver/windows.
- [4ee4e9ed39056d016bd882237edacb2f695a2d03](https://github.com/DarkIzuku/bloodborne_pc/commit/4ee4e9ed39056d016bd882237edacb2f695a2d03):
  only the notification queue and partial-unmap changes in `src/runtime_memory.c`.
  The upstream stability notices credit Awakenchan/BloodBorneButPc
  `bf594531a6de33ef607fe9661073c09e3a7fa9ff` for memory notification/remapping work.

Existing renderer notices remain GPL-2.0-or-later. New runtime adaptations and
the memory notification fixture retain the upstream GPL-3.0-or-later grant;
the license texts are included in this directory. Local fixture changes add
capture cleanup, Windows page-protection checks and explicit Vulkan dispatch.

No upstream renderer replacement, tiled writeback, DLSS preset change or frame
pacing policy is included in this batch.
