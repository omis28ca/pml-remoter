# Agent Handoff — ESP32 IR Hub

## What this project is
4-channel ESP32 IR blaster + 1 learning IR receiver. Control via web UI, REST, a
line-based TCP socket (port 4999), or MQTT. Learned commands persist in LittleFS
(`/commands.json`). Built on IRremoteESP8266. Not a git repo (no `.git` here) —
there is no commit history to consult; this file is the record of what happened.

## Current status
- **Builds clean.** Toolchain: PlatformIO was not installed in this environment;
  installed via `python -m pip install -U platformio` (lives under the user's
  `C:\Python314` / `AppData\Roaming\Python\Python314` install, not on PATH as `pio`).
  Build with `cd W:\src\esp32-ir-hub && python -m platformio run` (or `python -m
  platformio run -t upload` to flash a connected board).
- Flash usage: 73.8% (967,789 / 1,310,720 bytes). RAM: 14.7%.
- **Still never flashed to real hardware** — no board was attached this session.
  Everything below is "compiles and the embedded JSON parses correctly on a
  desktop Python json parser," not "verified against a real Comcast box."

## File map
| File | Role |
|---|---|
| `src/main.cpp` | Everything: Wi-Fi/AP, HTTP, TCP, MQTT, IR send/learn, LittleFS persistence |
| `include/config.h` | Pins, timeouts, buffer sizes |
| `include/webui.h` | Entire web UI as one embedded HTML/CSS/JS string (`INDEX_HTML`) |
| `include/presets_xfinity.h` | **New this session.** Built-in Comcast/Xfinity IR command table |
| `platformio.ini` | Pinned to `espressif32@^6.9.0` (Arduino-ESP32 2.x) — IRremoteESP8266 2.8.x needs the old ESP32 timer API; don't bump to platform 3.x without checking that first |

## What was done this session
1. **Set up the build.** Installed PlatformIO, ran `pio run` — it succeeded on the
   first try despite the README's old "not compiled or tested" warning (that
   warning is now stale; consider removing it if hardware gets verified).
2. **Added Comcast/Xfinity built-in presets** so the box can be commanded without
   a learning step first, while leaving the learning flow intact as an override.
   Source data: the "XFinity XR2 Remote" table from
   `github.com/MrJimmo/ComcastVizioRemote`'s README (fetched via `curl` against
   raw.githubusercontent.com).

### Design decisions (the "why", for whoever picks this up)
- **Presets are a separate `JsonDocument presetCmds`, not merged into
  `cmds`/`commands.json`.** `lookupCmd(name)` checks `cmds` first, falls back to
  `presetCmds`. This means: learning a command under a preset's name (e.g.
  `XF_CH_UP`) transparently overrides it, and deleting that learned command
  reverts to the built-in — without ever touching the preset data or requiring
  a "reset to default" code path. `hCommands()` reports `builtin`/`overridden`
  flags so the UI/API can show which is which.
- **New `"seq"` command shape**: `{"seq":[{code...},{"delayMs":500},{code...}]}`,
  sent once through in order, `repeat` ignored for the whole sequence. This
  exists because some Xfinity remote buttons (`ALL_POWER`, `GUIDE`, `XFINITY
  MENU`) genuinely send more than one IR code with a ~500ms gap — not something
  the old single-code-or-raw schema could express. This also happens to satisfy
  the "macros (sequences with delays)" item that was already on the README's
  TODO list, though only within one channel — a cross-channel macro is still
  unimplemented.
- **Naming: `XF_*` for box commands, `XF_TV_*` for the paired-TV NEC codes.**
  Deliberately namespaced so these never collide with whatever a user later
  learns for their own actual TV under a plain name like `POWER`.
- **Volume/mute are NOT built in as box (`XF_*`) commands.** On the real remote,
  those buttons send a no-op XMP code to the box and let the NEC code reach the
  TV/AVR instead — there's no meaningful "box volume" signal to capture. Only
  `XF_TV_VOL_UP/DOWN` and `XF_TV_MUTE` (NEC, TV-side) exist.
- **The `XF_TV_*` NEC codes are explicitly flagged as "probably-shared,
  unverified for your TV"** in both the README and the header comment — they
  were captured by the upstream project's author against one specific Vizio
  (remote code 11758), not derived from a Comcast/Xfinity spec. They're included
  because this particular NEC code family is commonly reused across many
  cable-box-compatible TVs, but that's a "worth trying first" claim, not a
  guarantee.

## Known gaps / untested / open questions
- **No hardware verification** of any preset code against a real Comcast/Xfinity
  box — this is a faithful transcription of a third-party capture, not
  something tested end-to-end here.
- `XF_ALL_POWER`'s delay values (150ms between NEC and the first XMP code) are
  an inserted guess for protocol-switch safety — the upstream README only
  documented the ~500ms gap between the two XMP codes, not a NEC→XMP gap.
  Worth tuning if `ALL_POWER` misbehaves.
- The old "not compiled or tested on hardware" line in README.md's Status
  banner is now misleading (the build issue it warned about didn't
  materialize) — flag to the user if they want it edited once real hardware
  testing happens.
- This directory has no `.git` — if the user wants history/diffing going
  forward, consider asking whether to `git init` it.

## If you're continuing this work
- Don't re-fetch the ComcastVizioRemote README unless verifying a detail — the
  full "XFinity XR2 Remote" and "Comcast Digital Adapter Remote" tables were
  already captured into `include/presets_xfinity.h` and this file.
- Rebuild/verify with `python -m platformio run` from the project root.
- If adding more presets (e.g. Vizio TV codes as their own `XF_VIZIO_*` preset
  set, or other STB generations), follow the same pattern: add to the
  `XFINITY_PRESETS_JSON` literal in `include/presets_xfinity.h`, keep names
  namespaced, don't touch `cmds`/override logic in `main.cpp`.
