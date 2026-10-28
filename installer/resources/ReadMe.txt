Automixer for DaVinci Resolve
=============================

Use
  Fairlight page: add Automixer to the mixer Effects slot of every mic track (not to the clips).
  Same Group = the mics that compete. Link on = they share their settings (Weight stays per mic).
  Amount: one control from gentle to aggressive. Presets: Save..., Delete, Reset.
  Order on the track: noise reduction -> Automixer -> compressor. Loudness on the bus.

Sync check (how two lav recordings line up)
  In Terminal:
    "/Library/Application Support/Automixer/automixer-sync-check" host.wav guest.wav
  It prints how far the guest file runs behind the host file for even sync (in seconds,
  and as frames + samples). With --offset <seconds> (your current placement) it also says
  how many samples to move the guest and in which direction.

Uninstall
  sudo "/Library/Application Support/Automixer/uninstall.sh"
  Your own presets in ~/Library/Application Support/Automixer/Presets are kept.

Support
  If Automixer saves you time: https://ko-fi.com/ilias823
