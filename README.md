# ESP32 IR Hub — 4 channels + learning receiver

Four independent IR outputs (each can drive a different device) and one IR receiver for learning.
Control it from a web UI, REST, a line-based TCP socket, or MQTT. Learned commands are stored in
flash (LittleFS) and survive reboots.

Built on [IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266) for protocol encode/decode.
Design ideas borrowed from qvinhdao/esp32_universalremote (multi-emitter + learning + web UI),
mdhiggins/ESP8266-HTTP-IR-Blaster (simple HTTP command format) and IRMQTTServer (MQTT layout).

> **Status:** written and reviewed but **not compiled or tested on hardware** (the build sandbox had no
> package access). Expect to fix a typo or two on first build.

## Build and flash

```
pio run -t upload          # firmware
pio device monitor
```

No filesystem upload is needed. The web UI is embedded in the firmware and LittleFS is formatted on first boot.
`platformio.ini` pins the Arduino-ESP32 2.x platform because IRremoteESP8266 2.8.x was written against the older timer API.

## First-time setup

1. With no saved Wi-Fi, the board starts an open access point named `IRHub-Setup`.
2. Join it and open `http://192.168.4.1`. Enter your Wi-Fi (and MQTT, if wanted) under **Settings**, then **Save & reboot**.
3. Afterwards use `http://irhub.local` or the IP the router gave it.

## Wiring (classic ESP32 DevKit; pins are in `include/config.h`)

| Function | GPIO |
|---|---|
| IR out channel 1 / 2 / 3 / 4 | 25 / 26 / 27 / 32 |
| IR receiver OUT | 34 (input-only pin, fine for a receiver) |
| Status LED | 2 |

**Receiver:** TSOP38238 or similar (38 kHz). VCC to 3V3, GND to GND, OUT to GPIO34.

**Each output channel (×4)**, do not drive an IR LED straight from a GPIO:

```
 5V ──┬── IR LED (940 nm) ── 47 Ω ──┐
      │                              │ collector
 GPIO ── 1 kΩ ── base   2N2222 / BC337 (NPN)
                       emitter ── GND
```

- 5 V with a 47 Ω resistor gives roughly 70 mA pulsed, which is good range for a rack. Use 100 Ω for ~35 mA, or lower the current if you
  tape an emitter onto a device's IR window.
- For equipment-rack use, a "blaster" stick-on emitter on a 3.5 mm jack per channel keeps each device isolated. Wire tip to the LED anode and sleeve to the collector.
- Keep the receiver away from the emitters. The receiver is only enabled during learning, and sends are refused while learning.

## Using it

**Learn:** open the web UI, type a name, press **Learn**, then aim the original remote at the receiver and press one button
(15 s timeout). Known protocols are stored as protocol + bits + value. Anything else is stored as raw pulse timings.
Repeat/hold frames are ignored.

**Send:** tap a channel number next to a command in the UI, or use any of the APIs below.

### Built-in Comcast/Xfinity presets — no learning required

The firmware ships with the IR command set for Comcast/Xfinity set-top boxes (XR2/XR11-era
remotes, XMP protocol), captured and published by
[MrJimmo/ComcastVizioRemote](https://github.com/MrJimmo/ComcastVizioRemote). Aim an output channel's
emitter at the box and send these names immediately — no learning step needed:

```
XF_POWER       box power toggle            XF_UP/DOWN/LEFT/RIGHT, XF_OK   nav + select
XF_ALL_POWER   TV + box power (multi-code) XF_EXIT, XF_LAST, XF_FAV
XF_CH_UP/DOWN  channel +/-                 XF_PAGE_UP/DOWN
XF_NUM_0..9    number pad                  XF_REWIND/PLAY/PAUSE/FF/STOP/REC   DVR transport
XF_GUIDE       on-screen guide             XF_SEARCH, XF_SWAP, XF_A/B/C/D
XF_MENU        Xfinity menu                XF_INFO
```

Plus a handful of NEC codes the XR2 uses to drive a *paired TV* (power/input/volume/mute) — these
are a widely shared code set across many TV brands, so they're worth trying even if your TV isn't
the Vizio they were originally captured against:

```
XF_TV_POWER   XF_TV_INPUT   XF_TV_VOL_UP   XF_TV_VOL_DOWN   XF_TV_MUTE
```

These are a fallback: `/api/send` and `SEND`/`SENDHEX` look up a name in the commands you've
learned first, and only use the built-in preset if nothing was learned under that name. So if a
particular box doesn't respond to a preset (different firmware/region), just **Learn** a command
under the same name (e.g. `XF_CH_UP`) and your learned version takes over — `DEL` (or the UI's
revert button) removes the override and falls back to the preset again. `/api/commands` reports
`"builtin"` and `"overridden"` flags so the UI can show which is which, and `LIST`/`STATUS` over TCP
report preset vs. learned counts the same way.

A command can also be a fixed **sequence**: `{"seq":[{...code...},{"delayMs":500},{...code...}]}`,
sent once through in order (repeat is ignored for sequences). `XF_ALL_POWER`, `XF_GUIDE` and
`XF_MENU` use this because the real remote sends more than one IR code with a pause in between for
those buttons — see `include/presets_xfinity.h`.

### REST

```
GET  /send?ch=1&cmd=TV_POWER&repeat=0                       plain GET, handy for simple controllers
POST /api/send   {"ch":1,"cmd":"TV_POWER","repeat":0}
POST /api/send   {"ch":2,"proto":"NEC","bits":32,"hex":"20DF10EF"}   ad-hoc, no need to learn first
POST /api/send   {"ch":3,"raw":[9000,4500,560,560,...],"hz":38}
POST /api/learn  {"name":"TV_POWER"}      GET /api/learn (poll state)     POST /api/learn/cancel
GET  /api/commands    POST /api/delete?name=X    GET /api/export    POST /api/import (JSON object)
GET  /api/status      GET/POST /api/config       POST /api/reboot
```

### TCP (port 4999, one command per line, CR and/or LF)

```
SEND 1 TV_POWER [repeat]            -> OK | ERR <reason>
SENDHEX 2 NEC 32 20DF10EF [repeat]  -> OK | ERR <reason>
LIST                                -> CMD <name> ... END
LEARN TV_POWER                      -> OK, later LEARNED <name> <summary> or LEARN_TIMEOUT (pushed to all clients)
CANCEL | DEL <name> | STATUS
```

Example from a control system: open a TCP socket to `irhub.local:4999` and send `SEND 1 TV_POWER\r`.

### MQTT (prefix configurable, default `irhub`)

| Topic | Direction | Payload |
|---|---|---|
| `irhub/ch/<1-4>/send` | in | command name, or JSON like `{"cmd":"TV_POWER","repeat":1}` or ad-hoc proto/raw |
| `irhub/learn` | in | name to store the next button press under |
| `irhub/learn/cancel` | in | any |
| `irhub/status` | out | `online` / `offline` (retained, last-will) |
| `irhub/event` | out | `{"event":"sent","ch":1,"cmd":"TV_POWER","ok":true}` |
| `irhub/learned`, `irhub/learn/status` | out | learn results |

## Limits and notes

- Protocols with long state (most air-conditioner remotes) are learned as raw timings, not decoded.
- Raw sends are bit-banged by the library, so each send briefly blocks the CPU (tens of milliseconds). Sends are serialized, never simultaneous across channels.
- There is no authentication. Keep it on a trusted LAN or VLAN.
- Settings changes (Wi-Fi, MQTT) apply after the automatic reboot.
- No Home Assistant auto-discovery yet; it's an easy addition on top of the MQTT layer.

## Ideas for next steps

Per-command default repeat counts, a "send on all channels" action, an OTA update endpoint, and a status LED per channel.
(Basic macros — a fixed sequence of codes and delays under one name — now exist via the `seq` command shape; a cross-channel version of that is still open.)
