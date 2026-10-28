# Automixer

A gain-sharing automixer VST3 for DaVinci Resolve (Fairlight) on macOS. Put one instance on each mic track. Every instance with the same **Group** links up with the others: the person talking stays open, the other mics drop by up to **Range**, and when people talk at the same time they share the gain.

Closed source, all rights reserved (see LICENSE). Universal (Apple Silicon and Intel), macOS 12+.

## Build and install

```
make test                  # build, run core tests and the VST3 host check
make install               # development copy in ~/Library/Audio/Plug-Ins/VST3
installer/build_pkg.sh     # dist/Automixer-<version>.pkg, the installer for other Macs
```
Restart Resolve after installing. The .pkg installs into `/Library/Audio/Plug-Ins/VST3` (scanned by Resolve by default) and removes a development copy from `~/Library`.

## Releases and updates

The source lives in the private repository. Installers are published in the public, releases-only repository `Ilias823/automixer-releases`, which the plugin's **Check for Updates** button reads (only when pressed).

1. Raise the version in `CMakeLists.txt` (`project(Automixer VERSION x.y.z)`).
2. Commit and push.
3. `installer/release.sh "What is new"`

## Sync check

`automixer-sync-check host.wav guest.wav [--offset seconds]` (installed in `/Library/Application Support/Automixer`) measures how far the guest file runs behind the host file for even sync, from the bleed of each voice on the other mic, plus the air path between the lavs.

## Controls

| Control | Default | What it does |
|---|---|---|
| Amount | 50 % | One bar from gentle to aggressive: sets Range, Focus, Hold and Release together. Fine-tune those afterwards |
| Group | A | Instances in the same group compete. Off = plain pass-through |
| Automix | on | Off: this mic stays at unity and the others ignore it |
| Link | on | Linked mics in a group share Amount, Range, Focus, Sensitivity, Attack, Hold, Release and the preset. Weight stays per mic. Off: this mic keeps its own settings; switching it back on takes the group's |
| Range | 18 dB | How far an inactive mic drops |
| Focus | 3.0x | How hard the loudest mic wins (1 = classic Dugan) |
| Sensitivity | 6 dB | How far above its own noise floor a mic must be to count as talking |
| Weight | 0 dB | Favour (+) or hold back (-) this mic |
| Attack / Hold / Release | 2 / 0 / 80 ms | Gain smoothing |

**Presets:** the Preset menu has factory presets (Default, Gentle, Aggressive, Noisy room) and your own. **Save...** stores the current values under a name in `~/Library/Application Support/Automixer/Presets`. **Delete** removes the selected preset of your own. **Reset** sets every value back to its default. Group, Automix and Name are per track and are never changed by presets or Reset.

The plugin adds a fixed latency (about three host blocks + 20 ms, ~52 ms in Resolve). Resolve compensates for it.

## Offline tuning

```
build/automix_cli_artefacts/Release/automix_cli host.wav guest.wav --start 120 --dur 60 --out out/
```
Writes the processed mics, `mix_raw.wav` and `mix_am.wav`, and `gains.csv`.
