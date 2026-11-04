# Techmo Control Hub

The Windows WPF frontend for `experiment-v2`.

| Page | Controls |
| --- | --- |
| Play | Game folder, launch, player profile and session status |
| Display | Resolution, frame rate, presentation, fullscreen and HDR |
| Graphics | Recommendations, upscaling, effects and camera |
| Controls | Keyboard/mouse slots, modifier capture, presets and controller |
| Trophies | Completion, earned/locked filters and notifications |
| Saves & mods | Save backups, import/export and mod selection |
| Online | Connection settings and sign-in |
| Diagnostics | Session reports, benchmark capture and results |
| Engine switches | Native engine overrides and defaults |
| Settings | Language, theme, motion and installation preferences |

Edits remain pending until Apply or Play. Undo restores the last applied state.
Recommendations show current and proposed settings before staging changes; they
use hardware information rather than a gameplay benchmark.

Configuration stays local in `launcher-settings.json`, `bbport.ini` and
`user/launcher/`. Saves default to `user/` and session reports to `logs/`.
These files are excluded from Git.

See [Build and verification](../../docs/BUILD.md) for source builds and CI artifacts.
