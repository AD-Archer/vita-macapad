# PS Vita Macapad docs

- [What it shows](#what-it-shows)
- [Requirements](#requirements)
- [Install](#install)
- [Using it](#using-it)
- [Using it as a controller](#using-it-as-a-controller)
- [Typing on your Mac](#typing-on-your-mac)
- [Keeping things awake](#keeping-things-awake)
- [Buttons](#buttons)
- [Files on the memory card](#files-on-the-memory-card)
- [Building](#building)
- [Releases](#releases)
- [Code](#code)
- [Emulator](#emulator)

## What it shows

- **The dashboard** (the first page): the time (from the Mac's clock), the weather where the Mac is, what's playing on
  the Mac (any app) with artwork, progress and previous / play / next buttons, and the Mac's CPU, memory, battery and
  disk.
- **Pages of keys**, 5×3 each, up to 12 pages. A key can run a Mac action (window halves and quarters, Siri,
  brightness, accessibility and more), open an app, open a website, press a keyboard shortcut or run a Shortcut. App keys show the app's icon; website keys show the site's icon (the
  icons the site's page declares, else its favicon). Each page's bar shows the time and the song too.

The song is polled every 1.5 seconds and is always current. Everything else is allowed to lag a little: the clock
offset, the weather and the Mac's numbers are kept on the memory card, so the dashboard is filled in from the first
frame and stays filled while the Mac is away (the song card says "Connecting…" until the Mac answers).

Song titles in Japanese, Korean or Chinese are drawn with the Vita's own system fonts; everything else is in Inter.

## Requirements

- [Macapad](https://ad-archer.github.io/Macapad/) 0.4 or later on your Mac (it needs `GET /api/status`). The Vita's
  "Find your Mac" screen shows a QR code for that page too. The controller, keys that repeat while held, Keep Mac
  awake and the mouse jiggler need Macapad 1.0.
- A PS Vita or PS TV that runs homebrew (HENkaku / Ensō) with VitaShell.
- The Vita and the Mac on the same Wi-Fi. A Vita can't join a tailnet.

## Install

In VitaShell, hold **←** on the D-pad and press **○** to open the QR code scanner, and scan this:

<img src="../assets/qr.png" width="160" alt="QR code for the newest macapad.vpk">

It always gets the newest release (`releases/latest/download/macapad.vpk`): VitaShell downloads it and asks whether to
install it, so scan it again to update. VitaShell downloads without signing in to GitHub, so the repository has to stay
public for this to work.

Or, without the camera (a PS TV has none):

1. Download `macapad.vpk` from the [releases](https://github.com/AD-Archer/vita-macapad/releases) page.
2. Copy it to the Vita: in VitaShell press **SELECT** to start its FTP server and upload it (or `just send <ip>`, see
   [Building](#building)), or plug in over USB.
3. Open it in VitaShell and choose **Install**. Macapad appears on the home screen.

Installing a newer VPK over it keeps your pages and pairing; they live in `ux0:data/macapad/`.

## Using it

1. **Find your Mac.** The Vita knocks on port 7317 of every address on your Wi-Fi and lists the Macs that answer. If
   yours doesn't show, choose **Type address** and enter its IP (`192.168.1.20`, or `192.168.1.20:7399` for another
   port).
2. **Pair.** Your Mac shows an Allow / Don't Allow panel with a 4-digit code. Check it matches the Vita and click
   Allow. The Vita shows up in Macapad's Settings as "&lt;your PSN name&gt;'s PS Vita".

   **Or scan a code:** on the Mac, choose **Pair with QR Code…** from Macapad's menu bar icon. On the Vita, choose
   **Scan code** and point the back camera at it from about a foot away. It pairs right away with nothing to approve
   on the Mac, and works even when the scan can't find the Mac. (A PS TV has no camera.)
3. **Press keys.** Swipe left from the dashboard (or press **R**) for the first page of keys. Tap a key to run it.
   Swipe, or **L** / **R**, to change pages. Tap an empty slot, or hold any key, to change it; the pencil in the bar
   turns on edit mode, where a tap on any key changes it.

   Picking what a key does leads to **How it looks**: the key as it will show, with its name, its icon (automatic, or
   any of the drawn glyphs) and its color (automatic, or one of nine). The same screen opens from **This key** when
   you hold a key that's already set; **What it does** on it goes back to the first question.

   The search field at the top of "New key" (or the magnifier in the title bar, or **△**) searches Mac actions, apps
   and Shortcuts at once. Each picker has the same field for searching just itself.

   The pickers open from a cache on the memory card, so there's no waiting after the first time. A thread keeps it
   fresh in the background: actions every 5 minutes, Shortcuts every 10, apps every 30. The last row of each picker
   (or **□**) asks the Mac again right away; **Refresh from your Mac** in the menu does that for all three and fetches
   the pad's icons again too. Icons are also cached on the memory card and fetched again when a week old.

The menu (the ☰ button in a page's bar, or **START**) jumps to a page, adds, renames, moves or removes pages, opens
the controller and the typing screen, switches Keep Mac awake and the mouse jiggler, switches the clock to 24 hours,
keeps this Vita awake (the default, so it works as a desk display), forgets the Mac, refreshes the lists and icons,
and quits.

Taps go to the Mac in order. Any made while one is still on its way are sent together in the next request, so a quick
run of taps doesn't wait a round trip each. If the Mac's address changes, the Vita looks for the same Mac on the
network again after a few failed polls.

## Using it as a controller

**Use as a controller** in the menu turns the Vita into a controller for the Mac: every button and both sticks press
keys or move the mouse, and the screen is a trackpad. Drag one finger to move the pointer (it speeds up when you move
quickly), tap to click, drag two fingers to scroll, tap with two to right-click. The screen shows what each button
does, lights it up while it's held, and says whether the Mac is getting it ("Connected"), or why not.

Hold **START** and **SELECT** together for a moment to leave (every other button goes to the Mac), or tap **‹ Done**.
**Set up** picks one of three presets:

| | Mouse & keys | Retro games | PC games |
|---|---|---|---|
| Left stick | moves the pointer | arrow keys | W A S D |
| Right stick | scrolls | nothing | mouse look (a quicker pointer) |
| D-pad | arrow keys | arrow keys | 1 2 3 4 (↑ → ↓ ←) |
| ✕ | click | Z | Space (jump) |
| ○ | right click | X | Shift (sprint) |
| □ | Space | A | E (use) |
| △ | Cmd+Tab (switch apps) | S | R (reload) |
| L / R | Cmd+[ / Cmd+] (back / forward) | Q / W | right click (aim) / click (shoot) |
| START / SELECT | Return / Esc | Return / Right Shift | Esc (menu) / Tab (map) |

"Retro games" is RetroArch's own keyboard setup, by where the buttons sit: the right face button is its A (X), the
bottom one B (Z), the top one X (S) and the left one Y (A). In "Mouse & keys", ✕ and ○ swap on consoles set up with
○ as confirm, so the confirm button clicks.

**Change a button** sets any button to a key, a click, keys held together (like Cmd+Tab), or nothing, and each stick to
the pointer, scrolling, arrow keys, W A S D, mouse look or nothing. Changes are kept per preset; **Back to preset**
undoes them. **Back touchpad scrolls** makes a finger on the back of the Vita scroll (off at first, so holding the
Vita doesn't scroll). The Vita stays awake while the controller is open.

The buttons go to the Mac as they're held (Macapad's live input, over UDP on its port), so a held key repeats and two
consoles can play at once. Nothing stays stuck: letting go, leaving the screen, the PS button, the Vita sleeping or the
Wi-Fi dropping all let go of everything on the Mac. The menu also has **Change the controller's buttons**, for setting
it up first (or without a touchscreen).

## Typing on your Mac

**Type on your Mac** in the menu: **Type something…** opens the Vita's keyboard, and what you type is typed on the Mac
where the cursor is. Under it are Esc, Tab, Space, Delete, Return and the arrows, which repeat while held, like a real
keyboard, and Command, Shift, Option and Control, which go with the next key (Command then Tab is Cmd+Tab). With an
older Macapad each tap presses the key once. **△** opens the keyboard too.

## Keeping things awake

- **Keep this Vita awake** (on by default): while Macapad is open the screen doesn't dim or turn off, and the Vita
  doesn't go to sleep. It tells the system once a second that someone's using it (`sceKernelPowerTick`).
- **Keep Mac awake** stops the Mac's screen from sleeping, and **Mouse jiggler** nudges its pointer when it's been idle,
  so it (and Slack, Teams…) never goes idle. Both show whether they're on right now, even when switched on the Mac or
  from another device. They're also Mac actions you can put on a key.

## Buttons

Everything works by touch, and everything works with the buttons too (for a PS TV, or anyone who'd rather).
✕ and ○ swap on consoles set up with ○ as confirm.

| Button | On the dashboard | On a page of keys | In a list |
|---|---|---|---|
| L / R | previous / next page | previous / next page | up / down a screenful |
| D-pad | ← → previous / next track, ↑ ↓ volume | move the focus (past the edge: next page) | move |
| ✕ | play / pause | press the key; hold to change it | pick |
| ○ | | leave edit mode | back |
| □ | play / pause | play / pause | refresh from the Mac |
| △ | mute | mute | search |
| SELECT | | edit mode | |
| START | menu | menu | close the menu |
| Sticks | | | scroll |

On the controller screen every button goes to the Mac (see [above](#using-it-as-a-controller)); hold START and
SELECT together to leave.

## Files on the memory card

Everything lives in `ux0:data/macapad/`:

| File | Holds |
|---|---|
| `config.json` | the Mac's address, this Vita's device token, the clock and screen settings |
| `pads.json` | your pages and keys |
| `controller.json` | the controller's preset, the buttons you changed in each, and whether the back touchpad scrolls |
| `status.json` | the last clock offset, weather and stats, for the dashboard before the Mac answers (per Mac) |
| `catalog-actions.json`, `catalog-apps.json`, `catalog-shortcuts.json` | the pickers' lists, with when they were fetched (per Mac) |
| `icons/<bundleId>.png` | cached app icons |
| `icons/web/<host>.png` | cached website icons |
| `net.log` | connections that failed, for working out network trouble (kept under 48 KB) |

Forgetting a Mac removes its `status.json` and catalogs; the icons stay.

`pads.json` is the same format as the [3DS client's](https://github.com/AD-Archer/3ds-macapad/blob/main/docs/README.md#files-on-the-sd-card),
with 15 keys a page instead of 12. Each key is `{kind, label, ref, category?, icon?, glyph?, color?}`:

| `kind` | `ref` is |
|---|---|
| `action` | a Macapad action id |
| `app` | a bundle id |
| `web` | a URL |
| `keys` | a key combo, like `cmd+shift+t` |
| `shortcut` | a Shortcut's name |

`icon` is the Material Symbols name the Mac suggests for an action; the Vita draws it when it has a matching glyph, and
built-in actions are drawn by id. `glyph` is the icon chosen in **How it looks** (a name like `play_arrow` or `lock`; it
wins over the automatic one), and `color` is `blue`, `green`, `orange`, `red`, `purple`, `yellow`, `teal`, `pink` or
`gray`.

## Building

The build runs in VitaSDK's Docker image, so the only thing to install is Docker Desktop (keep it running; on Apple
Silicon it runs the x86_64 image under emulation). `just` lists the tasks:

| Command | Does |
|---|---|
| `just build` | builds `macapad.vpk`, always clean |
| `just run` | builds and installs it in the Vita3K emulator, which opens it |
| `just mock` | runs a pretend Macapad on `127.0.0.1:7399` (see [Emulator](#emulator)) |
| `just send 10.0.0.42` | builds and uploads the VPK to `ux0:data/` over VitaShell's FTP (press **SELECT** in VitaShell first) |
| `just eboot 10.0.0.42` | once it's installed: builds and replaces only `eboot.bin` over FTP, the quick way to try a change |
| `just assets` | makes the LiveArea images in `sce_sys/` again from `assets/icon-source.png`, and `screenshots/livearea.png` |
| `just clean` | removes build output |

`sce_sys/` holds what the home screen shows: `icon0.png` (128×128), the LiveArea's `bg.png` (840×500) and
`startup.png` (280×158), and `template.xml`. The Vita only shows 8-bit indexed PNGs, which is what
`scripts/make-assets.py` writes.

<img src="../screenshots/livearea.png" width="420" alt="Macapad's LiveArea: the yellow face as the start button in a dark pad of keys, with the name to its right.">

The LiveArea is drawn as a pad of keys. The start button (`startup.png`) always sits in the same place, in the middle
and a little above center, so the keys are sized for it to cover exactly 3×2 of them, and its picture is cut from the
same scene with the face on top, so it blends in. The name is in the matching gap to its right. The icons on the keys
come from `ACCENTS` in the script.

The title id is `MCPD07317`. It's built as an "unsafe" app so it can keep its files in `ux0:data/macapad/`.

## Releases

`.github/workflows/build.yml` builds the VPK on every push and pull request, in VitaSDK's Docker image; it's in each
run's **Artifacts**.

Every push to `main` is also released as the next version, which the workflow tags itself: `v1.0`, `v1.1`, and so on.
The Vita keeps an app's version as two numbers (`APP_VER` is `XX.YY`), so the tags are two numbers as well, and after
`v1.99` comes `v2.0`. Put `[major]` in a commit message to go to the next major version sooner.

Each release carries `macapad.vpk`, `eboot.bin` and `macapad-qr.png`, and the newest is GitHub's "latest", so
`releases/latest/download/macapad.vpk` always gets the newest VPK. The release notes show the QR code with how to scan
it in VitaShell. The same image is kept at `assets/qr.png` for the README: the release job makes it again and commits
it if it ever changes (say, after renaming the repo).

## Code

| File | What |
|---|---|
| `src/main.c` | screens: find, scan code, pair, the dashboard and pages of keys (with swipes and the focus ring), key editor, grouped lists, menu, the controller and typing screens |
| `src/controller.c` | the controller's presets and changed buttons, and turning buttons, sticks and touch into keys, clicks and movement |
| `src/input.c` | live input: the input session, and the state of every key held sent over UDP (with acks, renewing the session, and letting go) |
| `src/macapad.c` | config and pads on the memory card, status / press / icon / catalog threads (presses batched, connections kept open), the status snapshot, scanning, pairing, the catalog cache |
| `src/http.c` | a small HTTP/1.1 client on the Vita's sockets, with keep-alive and Content-Length framing, plus the subnet scan |
| `src/gfx.c` | vita2d underneath: shapes from batched triangles, text in Inter (and the system fonts for CJK), the icon set, pictures |
| `src/qrscan.c` | the back camera at 640×480 straight into a texture, and a thread decoding QR codes with quirc, for pairing |
| `src/sys.c` | threads, locks and events on the Vita's kernel, the network, the on-screen keyboard, keeping the Vita awake, noticing it was away |
| `vendor/jansson/` | [jansson](https://github.com/akheron/jansson) 2.14, built here because VitaSDK's package is compiled as PIC, which `vita-elf-create` refuses (see `VENDORED.md`) |

The worker threads run on the Vita's second and third cores, so parsing answers and decoding images never cost the
screen a frame. Everything is drawn with 4× MSAA, so the rounded shapes stay smooth.

vita2d caches each glyph at the first size it's drawn at, so `gfx.c` keeps one font per size (they share the file in
memory).

## Emulator

[Vita3K](https://vita3k.org) runs it: `just run` installs the VPK and opens it, from `../.tools/vita3k-dl/Vita3K.app`
(download the macOS build into that folder). Its network calls reach the Mac's own network, so the Find screen works,
except that it skips its own address, which in the emulator is the Mac's.

To try the dashboard and keys without pairing your Mac, run `just mock` and put this in Vita3K's
`fs/ux0/data/macapad/config.json` (under `~/Library/Application Support/Vita3K/Vita3K/` on a Mac):

```json
{"url": "http://127.0.0.1:7399", "token": "mock-token", "macId": "mock-mac-0001", "macName": "Mock MacBook Pro"}
```

The pretend Mac plays a song (one with a Japanese title, to try the CJK fonts), reports weather and stats, serves app
and website icons, and prints every key press. It also gives out input sessions and prints the controller's UDP
packets as what's held changes; `--old` acts like a Macapad from before 1.0 and `--paused` like one that's paused. Without Sony's firmware installed in Vita3K, Japanese text shows as
boxes there; a real Vita has the fonts. The camera and the on-screen keyboard need real hardware.
