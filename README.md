# NeuralScreen — AMD version

**NVIDIA's DLSS 5 neural renderer, applied to your whole Windows desktop in
real time.** Everything on screen — games, video, photos — goes through the
same neural network that DLSS 5 games use, and comes back sharper.

**This fork adds Radeon.** The neural pass is no longer NVIDIA-only: on an
RX 7000 or RX 9000 card it runs for real, on the GPU, through a HIP engine
hosted inside the AMD worker. The NVIDIA path is untouched — same code, same
behaviour, same numbers. See [What the AMD version adds](#what-the-amd-version-adds).

> The guide below gets you running. How it works and what was measured:
> **[TECHNICAL.md](TECHNICAL.md)**; the AMD engine has its own write-up,
> **[docs/AMD_HIP_HOSTING.md](docs/AMD_HIP_HOSTING.md)**. Русская версия:
> **[README.ru.md](README.ru.md)** / **[TECHNICAL.ru.md](TECHNICAL.ru.md)**.

> **Notice.** Not affiliated with NVIDIA; NVIDIA, DLSS and the NVIDIA logo
> are NVIDIA Corporation's trademarks. The bundled NVIDIA runtimes
> (`nvngx_dlssnr.dll`, `nvngx_dlssg.dll`) are NVIDIA's property, included
> unmodified, research/educational use only, no warranty, use at your own
> risk. Rights holders: say the word and the next build ships without them.

## How it looks

<table>
<tr>
<td><img src="https://raw.githubusercontent.com/perseval-BLR/DLSS5-NeuralScreen/main/docs/screenshot-main-light.png" alt="Menu, light theme" width="400"></td>
<td><img src="https://raw.githubusercontent.com/perseval-BLR/DLSS5-NeuralScreen/main/docs/screenshot-main-dark.png" alt="Menu, dark theme" width="400"></td>
</tr>
<tr>
<td><img src="https://raw.githubusercontent.com/perseval-BLR/DLSS5-NeuralScreen/main/docs/screenshot-settings.png" alt="Settings" width="400"></td>
<td><img src="https://raw.githubusercontent.com/perseval-BLR/DLSS5-NeuralScreen/main/docs/screenshot-windows.png" alt="Window list" width="400"></td>
</tr>
</table>

*One menu inside the overlay, in a light and a dark theme; the settings page;
the window list — and the **Before / after wipe** slider that splits the
screen down the middle.*

## What you need

- **Windows 11**, or Windows 10 — reported working.
- **A card that can run the neural pass:**

  | Cards | Status |
  |---|---|
  | **RTX 50** / **RTX 40** / **RTX 30** | ✅ works |
  | **RTX 20** (Turing) | ❌ below the minimum architecture — the program starts, the picture is not processed |
  | **Hybrid laptops (Optimus)** | ✅ works; on the iGPU display the capture falls back to a slower path |
  | **Radeon RX 9000** (RDNA4) / **RX 7000** (RDNA3) | ✅ works, with files you supply yourself — see [What the AMD version adds](#what-the-amd-version-adds). Verified on an RX 9070 XT |
  | **Older Radeon, Intel, no dedicated GPU** | ⚠️ degraded mode: the program runs, the neural pass does not |

- **The latest NVIDIA driver, and Windows up to date.** Not a formality: the
  neural runtime talks to the driver directly, and an old driver is the
  commonest reason it refuses to start or the picture never appears.
- **Nothing installed.** The release archive brings its own Python.

### Without an NVIDIA card

The program detects your graphics hardware at startup — see the
`System using: ...` line under the status row in the menu, and in
`NeuralScreen.log` — and picks the backend from it:

- **NVIDIA present** — everything works as described here (30/40-series
  included, via the architecture hook documented in
  [TECHNICAL.md](TECHNICAL.md)).
- **Radeon RX 7000 / RX 9000** — the AMD worker takes over and the neural
  pass runs on the Radeon, once you have put the four files it needs in
  place. Until then it serves the picture through untouched and says so.
- **Anything else (Intel, older Radeon, no dedicated GPU)** — the program
  still opens, as a plain `NeuralScreen (degraded)` control window with the
  menu, tray icon, taskbar button and hotkeys, paced at your monitor's own
  refresh rate. The neural functions (NR, Boost, Frame Generation, recording,
  window/monitor/GPU switching) stay off and say so instead of failing:
  `Not available: worker missing`.

Degraded mode also covers a machine where the native files are missing
(`nvidia_mode/native/nvngx.dll`, `nvngx_dlssnr.dll`): same window, same disabled
functions, and `System using:` still names your card.

## What the AMD version adds

Upstream, a Radeon got a window with everything switched off. Here the
neural pass runs on it for real — because **Danielblnc**'s HIP engine, which
runs the DLSS-NR network on RDNA3/RDNA4, ships as a `version.dll` proxy that
hooks a game. So the AMD worker does not reimplement it: it *hosts* it.
`amd_nr_host.exe` builds a D3D12 device, a hidden swapchain and a real FSR
dispatch, and the runtime detours them exactly as it would a game's.

Two dispatches go out per frame: the one with motion vectors is the one the
runtime follows, so the network runs at the scale slider's resolution on an
unresampled frame; the second has none, so the runtime ignores it and **FSR**
upscales to your display. At full scale nothing is resampled at all.

Measured on an RX 9070 XT at 2560×1440: **14.7 fps** at scale 0.65 (the
network itself 23 ms, 43 ms at full scale), **+10%** fine detail on a sharp
photo, and the pass takes over ~3.5 s after launch — frames flow untouched
until then. It does what the network was trained for, **scene light and
detail**: skin, hair, fabric, contact shadows. On photographs, video and game
footage it shows; on a flat desktop it has nothing to add (measured: 0.56/255
of contribution on a menu against 6.1/255 on a photograph). That is the
domain, not a bug.

Also here: the menu's effect sliders reach the AMD engine (they did nothing
before), frames cross to the worker through shared memory rather than the
pipe, and the worker exits cleanly. How all of it was found out, and every
silent failure it steps around: **[docs/AMD_HIP_HOSTING.md](docs/AMD_HIP_HOSTING.md)**.

### Running it on a Radeon

The engine is a third party's binary and its weights derive from NVIDIA's, so
**none of it ships here**. Put your own copies in `amd_mode/weights/`:
`version.dll` (Danielblnc's runtime, the **standalone** build),
`dlssnr_on_amd_weights.bin`, and `amd_fidelityfx_upscaler_dx12.dll` (AMD's FSR
upscaler — the thing the runtime listens for). The `.ini` beside them is
written by the worker from your menu settings.

Then run with `NS_NR_BACKEND=amd`, or leave it on `auto` — with no NVIDIA card
the AMD worker is the one that can run. `NS_AMD_NR=0` forces the untouched
path for comparison; `NS_AMD_NR_SCALE_MAX` moves the top of the Intensity
slider (0.03 is where detail appears without artefacts, past 0.06 it looks
forced). Without those files nothing breaks: the worker names what is missing
and passes your frames through.

## Install

1. Download the archive from [Releases](https://github.com/perseval-BLR/DLSS5-NeuralScreen/releases)
   and unpack it anywhere. Everything is inside, including NVIDIA's runtime.
2. Run **`NeuralScreen.exe`**.

Windows will probably warn you about an unknown publisher — the program is not
signed with a paid certificate. Click *More info* → *Run anyway*, or use
`NeuralScreen.vbs` next to it.

There is no installer: to remove the program, delete the folder. Autostart is
the one thing written outside it — turn it off before you move or delete it.

> **Do not use it in competitive online games.** A fullscreen overlay over a
> game is what anti-cheat systems look for.

## Using it

The program sits in the tray and draws over your desktop. Press **Num2** for
the menu. The hotkeys are on the numpad, so **Num Lock has to be on**.

| Key | What it does |
|---|---|
| **Num2** | open / close the menu |
| **Num1** | neural rendering on / off |
| **Num7** | frame generation on / off |
| **Num3** | screenshot |
| **Num0** | start / stop recording, with sound |
| **Num4** / **Num6** | processing resolution down / up |
| **Num5** | capture the window under the cursor |
| **Ctrl+Alt+Q** | quit |

Every key can be reassigned in the menu, under the sliders icon. While the
menu is open it takes the mouse and keyboard, so it works on top of a game;
closed, clicks go straight through it.

### Whole screen or one window

The whole screen is the default. **Source**, second in the menu, switches
between **Fullscreen** and **Window mode**; choosing the second opens the list
of windows, and hovering a row highlights that window. **Num5** is the
shortcut when the window is already in front of you: point at it and press.
The overlay follows the window as it moves, and resizing it — a video going
fullscreen, a different player size — reconfigures the worker in place, with
no black moment. Minimising the window pauses processing.

## The menu

The dot next to your graphics card is green when neural rendering really runs
on it, red when it is not.

- **Source** — the whole screen or one window, and which window.
- **Profile** — how strong the effect is, from *Faithful* to *Extreme*;
  *Natural* by default. The four sliders underneath are the same thing in
  detail. **Save preset** stores the current values under a name and puts it
  in the Profile list; **Delete preset** removes it. Dark scenes are
  brightened automatically so shadows keep their detail. A profile moves the
  sliders only — the model below stays where you put it.
- **Model** — *which* network produces the picture, as opposed to how
  strongly. Three of them, and they are three different outputs rather than
  three strengths: **Default** suits a desktop, **Natural** and **Cinematic**
  are tuned for games and soften photographs and small text. Measured on a
  desktop capture, fine detail against the untouched frame: Default
  **+18.7%**, Natural **−11.4%**, Cinematic **−23.4%**. A saved preset keeps
  the model it was saved with.
- **Before / after wipe** — leaves the left part of the screen unprocessed so
  you can see what the effect is doing. Back to 0 when done.
- **Boost** — on by default. The network runs at a reduced resolution and a
  slider under the switch chooses which: measured on a 5070 Ti at 4K,
  **45.7 → 72.6 frames** at the default step and **83.4** at the lowest.
  The picture stays sharp — the network's result is composed onto your
  original frame, so text and edges keep full resolution. Turn it off to
  compare.
- **DLSS 4.5 FG** — Frame Generation, off by default, with a ×2 / ×3 / ×4
  multiplier beside the switch (and on **Num7**). DLSS-G's own desktop
  build: the depth is flat and the motion is estimated, there is no engine
  cooperation, so UI and text can distort — the known cost of the approach.
  The header pairs the two honest rates when they differ: "47 / 111 fps" is
  the network's output, then what the presenter shows. DLSS-G has a hardware
  floor of its own: on a card below Ada the runtime refuses and **the switch
  flips back off with a short notice** — no silent ON. Validated on RTX
  50-series; adapters beyond it are unconfirmed.

Everything else is behind the sliders icon: which monitor is processed and
which card does it, HDR compatibility, the screenshot folder, Spout2 output,
the recording indicator, leaving an unchanged screen alone, the key
assignments, the theme — and the language, of which there are **12**: English,
Russian, French, German, Spanish, Italian, Portuguese, Polish, Ukrainian,
Chinese, Japanese and Korean.

## Swapping a runtime

Everything ships in the archive. To run your own runtime build (a newer
DLSS-G, say), drop the DLL into **`nvidia_mode/native/libraries/`** — it wins over the
bundled copy; `nr_dll` / `NS_NR_DLL` remain the NR override.

## Recording and screenshots

**Num0** records what you see, with system sound, into an MP4 in
`recordings`. **Num3** saves a screenshot. The menu appears in both if it is
open, on purpose. A red dot with a timer sits in the corner while recording
(it can be turned off in the settings).

Screenshots freeze the processed frame before **Save As** opens, so the dialog
cannot appear in the image. Set **Screenshot folder...** once to start there.

**Recording externally:**

- **OBS (recommended):** turn on **Spout2 output (OBS)** in the settings, then
  add a **Spout2 Capture** source in OBS. Works in any mode.
- **NVIDIA App:** it has no Spout input, so use one-window mode. In full-screen
  mode the overlay intentionally hides from Windows/OBS capture to prevent a
  feedback loop; use Spout or the built-in screenshot for the processed frame.

## If something is not working

**Nothing appears after launch.** Check `NeuralScreen.log` next to the
program — it names the cause. The commonest is a missing
`nvidia_mode\native\nvngx_dlssnr.dll`.

**The overlay is invisible in a game.** True fullscreen cannot have anything
drawn over it — a Windows rule. Switch the game to *borderless*.

**The menu pointer is missing or frozen.** A fullscreen game hides the system
cursor, and the overlay only shows that one. Borderless fixes it.

**Everything is too bright and the sliders do nothing.** HDR is on for that
display. Turn it off (Win+Alt+B), or try **HDR compatibility** in the
settings — it is experimental; see [HDR setup](https://github.com/perseval-BLR/DLSS5-NeuralScreen/blob/main/docs/HDR.md).

**A key does nothing.** Something else claimed it; reassign it in the menu.

## Known limitations

- **True fullscreen games** cannot have an overlay drawn over them — borderless or windowed only.
- **HDR displays:** experimental, and off until you turn on **HDR compatibility** (settings, CAPTURE). Recording and Spout exports stay SDR. See [HDR setup and limitations](https://github.com/perseval-BLR/DLSS5-NeuralScreen/blob/main/docs/HDR.md).
- **Windows 10 and multi-GPU systems are experimental** — v1.12 fixes adapter/output selection from user logs, not local hardware. Reports welcome.
- **A rotated display:** 180° is turned back over on capture; 90° and 270° are not handled yet and come out with the sides swapped.
- **Pipeline latency** is 40–60 ms (17-20ms with Boost Mode) — fine interactively, not competitively; **processing resolution is capped at 2560×1440**, output is always your full native resolution.
- **Window mode:** panel blink and drag stutter were fixed in 1.11.0, taskbar
  reactivation in 1.12; the overlay can still drop behind on the first focus
  change.
- **On AMD:** the pass costs more than on NVIDIA — ~15 fps at 1440p against
  the NVIDIA path's 45–70 — because the frame still crosses Python twice
  and the hosted engine works at display resolution. It also only earns its
  keep on photographic content; a flat desktop gives it nothing to work
  with. Verified on one card (RX 9070 XT) and one driver.

## Where this is going

The AMD pass works by hosting someone else's binary, and that is the thing to
fix rather than celebrate. The engine is GPL-3.0-derived and ships without its
source, so a §6 request is written and ready to send
([docs/GPL_SOURCE_REQUEST.md](docs/GPL_SOURCE_REQUEST.md), one command:
`python tools/send_gpl_request.py`) — with the kernels and the graph in the
open, the AMD path could be built from source and actually shipped. Failing
that, a plan B that owes nobody: the weights container is already decoded
(153 tensors) and a DirectML executor is scaffolded; only the graph is
missing. Then frames: capture and display still cross Python every frame, so
doing the capture inside the worker is the next real win. Frame Generation on
AMD stays deferred — it needs Streamline, which is a project of its own.

Reports from other Radeon cards are welcome; this has only been run on an
RX 9070 XT.

## Credits

This version stands on other people's work, most of it given freely:

- **[perseval-BLR](https://github.com/perseval-BLR/DLSS5-NeuralScreen)** —
  NeuralScreen itself: capture pipeline, overlay, menu, NVIDIA worker, and
  everything this fork did not have to invent.
- **Danielblnc** — *DLSS-NR on AMD*, the HIP engine that makes a neural pass
  on Radeon possible at all. The AMD path here is a host for their work.
- **[wilsjo2](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)** —
  the OptiScaler AMD PreSR Multipass pack, and **permission (2026-09-16) to
  reuse the DlssNr design** it documents: multipass structure, parameter
  schema, bring-your-own-runtime model.
- **[OptiScaler](https://github.com/optiscaler/OptiScaler)** and
  **Dagherbou's Neural Rendering fork** — the upstream both packs build on.
- **AMD** — the FidelityFX SDK (MIT headers vendored in
  `amd_mode/third_party/ffx_api/`, and FSR does the upscaling here) and the
  HIP runtime the engine calls.
- **RenoDX** (colour work), **XeSS**, and **ShortFuse** (the cross-generation
  310.8 runtime that keeps RTX 20/30/40 in the picture).
- **NVIDIA** — DLSS and the runtimes, unmodified, under the notice above.
- **IBM Plex** — the interface faces (OFL-1.1).

Exact licences, hashes and the terms each piece arrived under:
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

The code here is MIT. NVIDIA's runtimes ship unmodified and remain NVIDIA's
property: `nvngx_dlssnr.dll` is the leaked 310.8.0 build (sm_75/86/89/120
kernels, RTX 20-50), `nvngx_dlssg.dll` is the public 310.9.1.0
redistributable — both as received, no guarantees, research-only. Interface
faces: IBM Plex (OFL-1.1, `fonts/OFL.txt`).

The AMD engine is **not** covered by any of that and is **not** distributed
here: it is a third party's binary, loaded from your own copy, for research
use. The code that hosts it (`amd_mode/native/`, `amd_mode/python/`) is MIT
and clean-room; anything derived from GPL-3.0 sources lives under
`amd_mode/third_party/` with its headers intact.
