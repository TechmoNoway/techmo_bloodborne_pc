# Source acknowledgements

The product uses the Techmo name. Original source authorship is retained here.

| Source | Acknowledgement |
| --- | --- |
| Lantern Engine, Awakenchan | Imported engine and frontend/configuration source, 0.9.34, commit `5321f86245567b4ae3c37d5ab4c6f362ef46a63b`. [Original repository](https://github.com/Awakenchan/LanternEngine). Includes shader compiler, draw pipeline, memory work, controls, upscaling, online integration and fixes. |
| Engine starting point | [DarkIzuku/bloodborne_pc](https://github.com/DarkIzuku/bloodborne_pc), built on [bbport](https://github.com/deadinside28/bloodborne_pc) and [shadPS4](https://github.com/shadps4-emu/shadPS4). Revisions are pinned in `engine.lock.json`. |
| VRAM cleanup and frame pacing | Adapted upstream from [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR). |
| Crash and controller fixes | [AwesomeObserver](https://github.com/AwesomeObserver/bloodborne_pc), [Supermedo](https://github.com/Supermedo/bloodborne_pc), [deadinside28](https://github.com/deadinside28/bloodborne_pc). |
| Cheats, sign-in, occlusion and trophy research | [droogie/bbhost](https://github.com/droogie/bbhost). |
| Online services | [The Hunter's Dream](https://thehuntersdream.com). |
| FSR 3.1 and FSR 4 on Vulkan | [FireBurn/FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK. |
| Optional FSR 4.1.1 | [fsr4vk](https://github.com/dvj5411/fsr4vk), with the AMD driver fix from [0xCydral](https://github.com/0xCydral/bloodborne_pc). |
| Optional DLSS | NVIDIA DLSS SDK. |
| Game patches | Kyo, Lance McDonald, auser1337, illusion, emoose and other community members. |
| Sound | [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9). |
| In-game menu | [Dear ImGui](https://github.com/ocornut/imgui). |
| Controller mappings | [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB). |
| Trophy list | [pyL1nx/bloodborne_pc](https://github.com/pyL1nx/bloodborne_pc). |
| UI library | [WPF-UI](https://github.com/lepoco/wpfui). |
| Performance capture | Intel PresentMon, obtained separately by the benchmark helper. |
| Packaged Python runtime | Python Software Foundation, Python 3.12.10 embeddable distribution; bundled license retained. |

The root `LICENSE` preserves GPL-2.0-or-later. Techmo additions retain their
GPL-3.0 notices in `Techmo-GPL-3.0.txt`. Detailed inherited notices remain in
`Techmo-THIRD_PARTY-NOTICES.txt`; native packages include `licenses/native/`.
Optional SDK binaries are not included by the source-build workflow.
