# curlog

Logs pointer movement and clicks on Wayland as JSONL until stopped
(SIGINT/SIGTERM). Raw input for [smoothcast](https://github.com/tihonove/smoothcast):
the video is recorded without the cursor (`wf-recorder-nc --no-cursor`), the
cursor separately.

```
curlog -o DP-2 [-f out.jsonl]          whole output
curlog -g "1920,0 800x600" [-f ...]    region, as from slurp
```

- Position comes from the compositor through an ext-image-copy-capture cursor
  session (sway ≥ 1.11 / wlroots ≥ 0.19). No extra permissions needed.
- Clicks come from `/dev/input`. The package installs a udev rule that opens
  mice, touchpads and trackpoints to the active session; keyboards stay closed.
  Touchpad taps are not visible.
- Format and clock are described at the top of `curlog.c`. The last line is
  `{"ev":"stop"}`: the video and the log are aligned by their ends.

## Install

From the apt repository [tihonove/apt](https://github.com/tihonove/apt), package `curlog`.
From source: `make && sudo make install` (needs `libwayland-dev`, `wayland-scanner`).
