# Windows guest red-zone protection

The pinned Bloodborne 1.09 site coordinates and instruction lengths in
`scripts/red_zone_sites.json` come from d4rksp4rt4n's analysis, published in
[Supermedo/bloodborne_pc PR #2](https://github.com/Supermedo/bloodborne_pc/pull/2),
commit `bc893e25eb961182ad9ddd455d3234de743dbc04`, file `patches/redzone.json`.
Its exact SHA-256 is
`10ae6c6f891c3ed8be71fa8e92ce7236843d07a2b5d5813268eab74b52692e28`.
The upstream repository retains the GNU GPL version 2 license, included here
as `GPL-2.0.txt`. The table is unmodified; original game executables are not
included. Copyright and license notices of upstream components remain intact.

The protection mechanism follows the Windows/System V ABI work in
[shadPS4 #4802](https://github.com/shadps4-emu/shadPS4/pull/4802)
(GPL-2.0-or-later). Techmo's standard-library implementation in
`scripts/red_zone.py` adds plan integrity/structure checks, executable range
checks, transactional detours, selected patch overlap checks and diagnostics.

The upstream table covers 607 spans in 153 analyzed leaf functions. It excludes
jump-table functions and community-patched functions and lists 42 short stores
that cannot hold a rel32 jump. Changed bytes, relocations and selected startup
patches can further reduce coverage; the linker records each skipped site.
Protection does not imply that all guest memory operations are covered.
