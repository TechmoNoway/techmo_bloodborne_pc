# Dependency provenance

Pinned source snapshots are included in this repository. Clones and source ZIPs
include the dependencies needed by the build; there are no Git submodules.
Keep upstream notices with these directories when updating them.

| Component | Location | Source revision | License |
| --- | --- | --- | --- |
| FSR-Vulkan | `engine/renderer/third_party/fsr-vulkan` | `FireBurn/FSR-Vulkan` · `c64f093404125e960813f95f1996c14780f0dd69` | MIT; see its LICENSE/NOTICE |
| Dear ImGui | `engine/renderer/third_party/imgui` | `ocornut/imgui` · `f1cc2ae15e53a861a874c3034aae6798fde194ab` | MIT |
| LibAtrac9 | `third_party/LibAtrac9` | `Thealexbarney/LibAtrac9` · `efca2e3af35562a09a9bb6deed90e45b4b824dc4` | See component LICENSE |
| shadPS4-derived renderer | `engine/renderer/shadps4` | `b4e7ae776071ceb1dd35004577a8719481e1d258`, plus listed backports | GPL-2.0-or-later |
| bbhost ports | Runtime/renderer engine and input modules | Revisions listed in `licenses/BBHOST-PORTS.md` | GPL-3.0-or-later |

The LibAtrac9 snapshot revision is recorded from the original source checkout.
Other retained dependencies include sirit, GCN headers, half, DejaVu fonts,
cpp-httplib and wepoll. Their notices remain in their component directories and
`licenses/`. The shadNet adaptation is documented in `licenses/SHADNET-PORT.md`.

The engine build applies the port's existing FSR-Vulkan patches from
`engine/renderer/patches/fsr-vulkan`. Those patches and upstream notices remain
reviewable independently of the vendored snapshot.

The vendored ImGui Android example's ignore rules allow its existing Gradle
version catalog while excluding generated wrappers. The root ignore rules also
preserve SPIRV-Headers' header-generation scripts in its source `bin` directory.

CMake also fetches pinned versions of magic_enum (v0.9.7), miniz (3.1.0) and
xbyak (v7.24.2) when missing. MSYS2 supplies the compiler, Vulkan, SDL3 and other
system libraries listed in the [build guide](docs/guides/BUILD.md).

DLSS and FSR model downloads are optional runtime assets with their own notices;
downloaded DLLs/model sets are excluded from this source repository.
