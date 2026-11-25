#pragma once
#include <pgmspace.h>

// Built-in IR command presets for Comcast/Xfinity set-top boxes (XR2/XR11-era
// remotes) and the handful of common NEC codes those remotes use to also
// drive a paired TV. Captured and published by MrJimmo:
//   https://github.com/MrJimmo/ComcastVizioRemote
//
// These are parsed into `presetCmds` at boot (see loadPresets() in main.cpp)
// and looked up as a fallback whenever a name isn't found in the user's
// learned /commands.json. Learning a command under one of these names stores
// it in /commands.json, which then takes priority over the preset of the
// same name -- so a box that doesn't match these defaults can still be
// learned and will simply shadow the built-in.
//
// Naming:
//   XF_*      -> sent straight to the set-top box (XMP protocol, 64-bit).
//   XF_TV_*   -> NEC codes the XR2 uses for the *paired TV* (power, input,
//                volume, mute). These happen to be a very widely shared NEC
//                code set used by many cable-box remotes across TV brands;
//                they were captured against a Vizio (remote code 11758) but
//                are a reasonable first thing to try against other TVs too.
//                If they don't work for your TV, just learn the real remote
//                under the same name (e.g. XF_TV_POWER) to override them.
//   XF_ALL_POWER, XF_GUIDE, XF_MENU -> multi-step "seq" commands: the real
//                remote sends more than one IR code with a pause in between
//                for these buttons. See doSend()'s "seq" handling in main.cpp.
//
// Volume/mute aren't present as box-only (XF_*) commands: on the real
// remote those buttons send a no-op XMP code to the box and let the NEC
// code reach the TV/AVR instead, so there's nothing useful to send the box.
static const char XFINITY_PRESETS_JSON[] PROGMEM = R"json({
"XF_POWER":{"proto":"XMP","bits":64,"hex":"170F443E10000F00"},
"XF_ALL_POWER":{"seq":[{"proto":"NEC","bits":32,"hex":"20DF10EF"},{"delayMs":150},{"proto":"XMP","bits":64,"hex":"170F443E10000F00"},{"delayMs":500},{"proto":"XMP","bits":64,"hex":"170F443E1100E000"}]},
"XF_CH_UP":{"proto":"XMP","bits":64,"hex":"170F443E12000D00"},
"XF_CH_DOWN":{"proto":"XMP","bits":64,"hex":"170F443E11000E00"},
"XF_NUM_0":{"proto":"XMP","bits":64,"hex":"170F443E1F000000"},
"XF_NUM_1":{"proto":"XMP","bits":64,"hex":"170F443E1E000100"},
"XF_NUM_2":{"proto":"XMP","bits":64,"hex":"170F443E1D000200"},
"XF_NUM_3":{"proto":"XMP","bits":64,"hex":"170F443E1C000300"},
"XF_NUM_4":{"proto":"XMP","bits":64,"hex":"170F443E1B000400"},
"XF_NUM_5":{"proto":"XMP","bits":64,"hex":"170F443E1A000500"},
"XF_NUM_6":{"proto":"XMP","bits":64,"hex":"170F443E19000600"},
"XF_NUM_7":{"proto":"XMP","bits":64,"hex":"170F443E18000700"},
"XF_NUM_8":{"proto":"XMP","bits":64,"hex":"170F443E17000800"},
"XF_NUM_9":{"proto":"XMP","bits":64,"hex":"170F443E16000900"},
"XF_OK":{"proto":"XMP","bits":64,"hex":"170F443E18002500"},
"XF_UP":{"proto":"XMP","bits":64,"hex":"170F443E1C002100"},
"XF_DOWN":{"proto":"XMP","bits":64,"hex":"170F443E1B002200"},
"XF_LEFT":{"proto":"XMP","bits":64,"hex":"170F443E1A002300"},
"XF_RIGHT":{"proto":"XMP","bits":64,"hex":"170F443E19002400"},
"XF_EXIT":{"proto":"XMP","bits":64,"hex":"170F443E13002A00"},
"XF_INFO":{"proto":"XMP","bits":64,"hex":"170F443E17002600"},
"XF_GUIDE":{"seq":[{"proto":"XMP","bits":64,"hex":"170F443E16002700"},{"delayMs":500},{"proto":"XMP","bits":64,"hex":"170F443E1100E000"}]},
"XF_MENU":{"seq":[{"proto":"XMP","bits":64,"hex":"170F443E1D002000"},{"delayMs":500},{"proto":"XMP","bits":64,"hex":"170F443E1100E000"}]},
"XF_LAST":{"proto":"XMP","bits":64,"hex":"170F443E19005100"},
"XF_FAV":{"proto":"XMP","bits":64,"hex":"170F443E18005200"},
"XF_PAGE_UP":{"proto":"XMP","bits":64,"hex":"170F443E15002800"},
"XF_PAGE_DOWN":{"proto":"XMP","bits":64,"hex":"170F443E14002900"},
"XF_REWIND":{"proto":"XMP","bits":64,"hex":"170F443E19003300"},
"XF_PLAY":{"proto":"XMP","bits":64,"hex":"170F443E1C003000"},
"XF_PAUSE":{"proto":"XMP","bits":64,"hex":"170F443E1A003200"},
"XF_FF":{"proto":"XMP","bits":64,"hex":"170F443E18003400"},
"XF_STOP":{"proto":"XMP","bits":64,"hex":"170F443E1B003100"},
"XF_REC":{"proto":"XMP","bits":64,"hex":"170F443E17003500"},
"XF_SEARCH":{"proto":"XMP","bits":64,"hex":"170F443E1400CF00"},
"XF_SWAP":{"proto":"XMP","bits":64,"hex":"170F443E11005900"},
"XF_A":{"proto":"XMP","bits":64,"hex":"170F443E19006000"},
"XF_B":{"proto":"XMP","bits":64,"hex":"170F443E18006100"},
"XF_C":{"proto":"XMP","bits":64,"hex":"170F443E17006200"},
"XF_D":{"proto":"XMP","bits":64,"hex":"170F443E15008200"},
"XF_TV_POWER":{"proto":"NEC","bits":32,"hex":"20DF10EF"},
"XF_TV_INPUT":{"proto":"NEC","bits":32,"hex":"20DFF40B"},
"XF_TV_VOL_UP":{"proto":"NEC","bits":32,"hex":"20DF40BF"},
"XF_TV_VOL_DOWN":{"proto":"NEC","bits":32,"hex":"20DFC03F"},
"XF_TV_MUTE":{"proto":"NEC","bits":32,"hex":"20DF906F"}
})json";
