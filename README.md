# wl-castlog

Logs pointer movement and clicks on Wayland as JSONL until stopped
(SIGINT/SIGTERM). Raw input for screencast post-processing: the video is
recorded without the cursor (e.g. `wf-recorder-nc --no-cursor`), the cursor
separately, and a tool like [smoothcast](https://github.com/tihonove/smoothcast)
draws it back.

```
wl-castlog -o DP-2 [-f out.jsonl]          whole output
wl-castlog -g "1920,0 800x600" [-f ...]    region, as from slurp
```

- Position comes from the compositor through an ext-image-copy-capture cursor
  session (sway ≥ 1.11 / wlroots ≥ 0.19). No extra permissions needed.
- Clicks come from `/dev/input`. The package installs a udev rule that opens
  mice, touchpads and trackpoints to the active session; keyboards stay closed.
  Touchpad taps are not visible.
- Format and clock are described at the top of `wl-castlog.c`. The last line is
  `{"ev":"stop"}`: the video and the log are aligned by their ends.

## Install

From the apt repository [tihonove/apt](https://github.com/tihonove/apt), package `wl-castlog`.
From source: `make && sudo make install` (needs `libwayland-dev`, `wayland-scanner`).

## Release

`./release` (or `./release minor|major|X.Y.Z`) tags the next version, waits for
CI to build the packages and publishes the apt repository.
