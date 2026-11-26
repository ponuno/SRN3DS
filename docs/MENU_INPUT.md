# StartMenu input profile (3DS HID → TESV guest)

Status: **control mapping implemented and host-tested; no menu frame has been
executed and no in-engine navigation is claimed.** The retail path in
`F:\Skyrim\TESV.exe` (1.9.32.0.8) is static evidence only.

## 1. How StartMenu receives input in TESV

Verified from the PE import table (checked by `tools/menu_harness.py --exe`,
`input.*` rows) and the earlier ControlMap trace in `docs/PORT_LOG.md` rows
108–109:

| Evidence | Meaning |
|---|---|
| `DINPUT8.dll!DirectInput8Create` (IAT `0x0106b01c`) | keyboard/mouse devices come from DirectInput8, not the message queue |
| `USER32.dll!GetAsyncKeyState` (IAT `0x0106b2bc`) | per-frame key edge polling on the same virtual state |
| `USER32.dll!GetKeyState` (IAT `0x0106b324`) | latched key state; call site `0x00a6aed8` (`push 0x90` = `VK_NUMLOCK`) |
| `USER32.dll!GetCursorPos` (IAT `0x0106b328`) | absolute cursor in UI hit-testing |
| `USER32.dll!MapVirtualKeyA` / `ToUnicode` | text entry conversions |
| `Interface/Controls/PC/ControlMap.txt` in `Skyrim - Interface.bsa` | the engine's own key→action bindings, loaded at `0xa68340` (row 109) |
| `PeekMessageA` is imported but our shim never fabricates a message | menus must not rely on `WM_KEYDOWN`; the DirectInput state is the real path |

No `GetKeyboardState`, no Raw Input and no named `XInput*` import exists for
input; `XINPUT1_3.dll` is only ordinal-resolved by the shim. Therefore the
minimal correct contract is: feed the DirectInput keyboard state (256-byte DIK
array), mouse axes/buttons and the user32 virtual-key queries from one HID
sample. `source/win32_input.c` already did this before this change; it now also
serves `GetKeyState` instead of the old `return 0` stub in `source/win32_user.c`
(the input module resolves first in `win32_lookup`).

The StartMenu SWF itself listens to the game's menu events (Up/Down/Accept/
Cancel, fed from ControlMap); this layer does not invent event names. It supplies
both the arrow keys and WASD for range movement, because retail ControlMap may
bind either set, and Enter/Escape/Tab for accept/cancel/back.

## 2. 3DS → guest mapping

`Win32InputProfile` in `source/win32_input.h`; the single table is
`win32_input_map_pad()`, used both by the libctru sampler (`hidScanInput`) and by
the host test.

Profile `menu` (default):

| 3DS input | guest codes | DirectInput scan code |
|---|---|---|
| D-pad Up/Down/Left/Right | `VK_UP`/`VK_DOWN`/`VK_LEFT`/`VK_RIGHT` **and** `W`/`S`/`A`/`D` | `0x48`/`0x50`/`0x4B`/`0x4D`, `0x11`/`0x1F`/`0x1E`/`0x20` |
| A | `VK_RETURN` (0x0D) — **not** a mouse click | `0x1C` |
| B | `VK_ESCAPE` (0x1B) | `0x01` |
| Start | `VK_ESCAPE` (0x1B) | `0x01` |
| Select | `VK_TAB` (0x09) (tween/back) | `0x0F` |
| X | `VK_SPACE` (0x20) | `0x39` |
| Y | `E` (activate alternative) | `0x12` |
| L / R / ZL / ZR | `VK_LSHIFT` / `R` / `VK_RCONTROL` / `VK_RMENU` | `0x2A` / `0x13` / `0x1D` / `0x38` |
| touch | absolute cursor (`400×240` mapping) + left button (`0x01`) | `DIMOFS_X/Y`, button 0 |
| circle pad / C-stick | mouse relative motion / right-stick axes (unchanged) | — |

Profile `legacy` keeps the pre-profile behaviour for older gameplay probes:
A/B/X are left/right/middle mouse buttons, D-pad is WASD, Start is Escape,
Select is Tab.

Selection:

```c
win32_input_set_profile("menu" | "legacy");  /* 0 ok, -1 = named refusal */
win32_input_apply_pad(WIN32_INPUT_PAD_A);    /* host/diagnostic seam */
```

or at startup:

```text
TESV_INPUT_PROFILE=menu     # default
TESV_INPUT_PROFILE=legacy   # old mapping
```

An unknown value prints
`[input] unknown control profile "<x>" (expected menu|legacy); keeping "<y>"`
and is not silently accepted. `Win32InputProfile`, the `WIN32_INPUT_PAD_*`
bits and `win32_input_map_pad()` are public so no test needs libctru.

## 3. Host evidence

`tests/win32_input_smoke.c` (target `input` in `tools/run_host_tests.ps1`):

* pure mapping: menu A → `0x0D` with no mouse-button bit; D-pad → arrows + WASD;
  B/Start → Escape; Select → Tab; X → Space; Y → E; legacy A → left mouse and no
  Enter; legacy D-pad → WASD and no arrow;
* end-to-end through the guest thunks: `apply_pad(PAD_A)` → DirectInput
  `GetDeviceState` byte `0x1C == 0x80`, `GetAsyncKeyState(0x0D) & 0x8000`,
  `GetKeyState(0x0D) & 0x8000`, and all bits clear after release;
* profile by name, including the named refusal for an unknown name.

`tests/menu_harness_test.py` now also requires the four real input imports when
`TESV_EXE` is present.

## 4. Harness use once the frame exists

Set `TESV_INPUT_PROFILE=menu` (already in the harness env printed by
`tools/menu_harness.py`) and launch normally. The scene to look for:

1. **Main menu draws** (New / Continue / Quit / Credits). This is *not* claimed
   yet; `docs/MENU_HARNESS.md` lists the GFx/first-draw blockers.
2. **D-pad Up/Down** moves the highlight one row per press (no repeat expected
   from a single press); Left/Right must not change the selected row.
3. **A** accepts the highlighted item. On the first screen that means entering
   the New-game flow or the Continue list; if a save list appears, Up/Down and A
   must work there too.
4. **B or Start** returns one level (Escape). From the root menu Escape may be
   ignored by the game (it is the top level); that is engine policy, not an
   input failure.
5. **Touch** on "Quit" selects and confirms with one tap (absolute cursor +
   left click); touching elsewhere must not confirm. Mouse-only text hover must
   track the finger position mapped to `400×240`.
6. **Quit** must reach the confirmation dialog, then B cancels and A confirms.

What to archive for each check: screenshot plus
`sdmc:/Skyrim_N3DS_stderr.log`. A missing reaction must be diagnosed from the
log/state: `[input] unknown control profile` means the env was mistyped; a
highlight that never moves with both arrows and WASD means `GetDeviceState` is
not being called (GFx/event layer, not the HID layer); a moved highlight with a
dead A means the ControlMap's accept binding is not Enter and the next step is
reading `ControlMap.txt` from `Skyrim - Interface.bsa` rather than adding more
keys speculatively.

## 5. Limits / open items

* No menu frame, no GFx event dispatch and no screenshot exist; every statement
  above is host mapping + static import evidence.
* The exact `ControlMap.txt` accept/cancel bindings are not yet extracted and
  read; the profile deliberately covers the standard PC set (arrows/WASD,
  Enter, Escape, Tab).
* `GetKeyState` toggle bits only change through injected Caps/Num/Scroll keys;
  the 3DS has none. No `GetKeyboardState` import exists in TESV.
* DirectInput buffered data is still an empty queue (`GetDeviceData` returns 0);
  the engine is served by `GetDeviceState`, matching the retail menu path.
