# Automixer

A gain-sharing automixer VST3 for DaVinci Resolve (Fairlight) on macOS. Put one instance on each mic track. Every instance with the same **Group** links up with the others: the person talking stays open, the other mics drop by up to **Range**, and when people talk at the same time they share the gain.

Free and open source. If it saves you time, you can support its development on **[Ko-fi](https://ko-fi.com/ilias823)**.

Universal (Apple Silicon and Intel), macOS 12+. Unlike other automixers, every setting is saved with the Resolve project.

## Install

1. Download `Automixer-<version>.dmg` (or the `.pkg`) from the [latest release](https://github.com/Ilias823/automixer/releases/latest).
2. Open it and run **Install Automixer.pkg**. The installer isn't signed by Apple: if macOS blocks it, right-click it and choose **Open**, or allow it in System Settings → Privacy & Security.
3. Restart DaVinci Resolve. In Fairlight, add **Automixer** to each mic track and give them the same Group.

To remove it, run `/Library/Application Support/Automixer/uninstall.sh`.

## Build from source

Needs Xcode and CMake (`brew install cmake`); JUCE is downloaded by CMake.

```
make test                  # build, run core tests and the VST3 host check
make install               # development copy in ~/Library/Audio/Plug-Ins/VST3
installer/build_pkg.sh     # dist/Automixer-<version>.pkg, the installer for other Macs
```
Restart Resolve after installing. The .pkg installs into `/Library/Audio/Plug-Ins/VST3` (scanned by Resolve by default) and removes a development copy from `~/Library`.

## Releases and updates

Installers (.pkg and .dmg) are published as GitHub releases of this repository, which the plugin's **Check for Updates** button reads (only when pressed; nothing is sent otherwise).

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

## Support and licence

Automixer is free and open source, licensed under the [GNU General Public License v3.0](LICENSE). It is built with [JUCE](https://juce.com), used under its AGPLv3 licence, and the VST3 SDK (MIT). If it saves you time, you can [support it on Ko-fi](https://ko-fi.com/ilias823). The plugin's **Support on Ko-fi** button and the installer's last page link there too; nothing pops up on its own.

VST is a registered trademark of Steinberg Media Technologies GmbH.
