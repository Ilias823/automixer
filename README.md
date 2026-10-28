# Automixer

A gain-sharing automixer VST3 for DaVinci Resolve (Fairlight) on macOS. Put one instance on each mic track. Every instance with the same **Group** links up with the others: the person talking stays open, the other mics drop by up to **Range**, and when people talk at the same time they share the gain.

## Build and install

```
make test      # build, run core tests and the VST3 host check
make install   # copy to ~/Library/Audio/Plug-Ins/VST3 and verify
```
Restart Resolve after installing.

## Controls

| Control | Default | What it does |
|---|---|---|
| Group | A | Instances in the same group compete. Off = plain pass-through |
| Automix | on | Off: this mic stays at unity and the others ignore it |
| Range | 18 dB | How far an inactive mic drops |
| Focus | 3.0x | How hard the loudest mic wins (1 = classic Dugan) |
| Sensitivity | 6 dB | How far above its own noise floor a mic must be to count as talking |
| Weight | 0 dB | Favour (+) or hold back (-) this mic |
| Attack / Hold / Release | 2 / 0 / 80 ms | Gain smoothing |

The plugin adds a fixed latency (about one host block + 20 ms). Resolve compensates for it.

## Offline tuning

```
build/automix_cli_artefacts/Release/automix_cli host.wav guest.wav --start 120 --dur 60 --out out/
```
Writes the processed mics, `mix_raw.wav` and `mix_am.wav`, and `gains.csv`.
