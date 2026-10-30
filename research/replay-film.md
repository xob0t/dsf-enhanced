# Film Director's "old film" look

Film Director plays replays back with a look of its own: the picture goes grey-green and washed
out, and scratches, dust and hairs come and go. `[Replay] DisableFilmEffect` (`src/replay_film.inl`)
leaves it out. This note says what the look is and why it is skipped by texture.

Evidence from VM runs (VMware SVGA, native D3D9, 1280x720) on the user's 2:30 clip played as
"latest footage" (`dng.replay`), with a test build that started playback from the main menu (the
two calls Film Director's own button makes, below) and logged the tone map and the HUD pass.

## It is a GUI page, not post-processing

The tone map (`PostProcessing.fx`, pixel shader `2F0359F4`, research/dlss5-renderer.md) gets the
same inputs in replay playback as in normal play. Logged on its draw every 60 frames:

| | Normal play (Story, street level) | Replay playback, driving |
| --- | --- | --- |
| Colour grade (`s3`/`s4`, 16³ LUTs) | `CECA31FD`, grey axis 040507 322B2A B19E96 E3D3CE F2E7E0 | `5DC00527`, grey axis 040507 312B2A AF9E97 E2D4CF F1E7E1 |
| Vignette `c4` | (1, 1) Shift view, 0.6 on the street (ClearDay's `ppeEdgeVignette`) | (0.6, 1) |
| Grain `c2`, noise texture `s5` | (0.25, 0.3, 1, 0), 128x128 DXT1 | the same |

(LUTs are hashed over their bytes; the grey axis is the LUT at (k, k, k) for k = 0, 4, 8, 12, 15.)
The replay's LUT is the game's grade, blended a little differently, so the yellow grade is intact
before the HUD pass.

The look is drawn in the composite + HUD pass (render-queue target set 0, `T#1`) by the
`Replays` menus scene, from `Media\GUI_FilmDirector.fchunk` (`Pages\grain_and_scratches`,
`Img.Replay_tint`, `grain_black_*`, `base_scratch`, `black_scratch`). Per draw, in a driving
frame with the playback menu shown:

| Draw | Texture at stage 0 | What |
| --- | --- | --- |
| 1 | | the composite (MLAA neighbourhood blend), as in normal play |
| 2 | 4x1 white | changes no pixel (the dumps after draws 1 and 2 are identical) |
| 3 | 8x8 DXT5, pale cyan (203, 255, 233) | full-screen tint: the darkest corner went from (8, 6, 6) to (33, 38, 35), the frame's mean by +11/+18/+16 |
| 4-35 | the menu's atlas, icons, text | the playback menu (X hides it) |
| last few | 8x800, 32x32, 64x64 DXT5 and 256x512 DXT1 | a long vertical scratch, a dust speck, a hair, an atlas of scratches and specks; not every frame |
| last | 32x32 | the mouse cursor |

Hiding the menu with X (the scene's `hideMenu` variable) leaves the tint and the damage sprites.

## Why by texture

- **No engine flag found.** Starting playback (`FUN_00795EE0`, the `Replay` launch state) sets no
  post-processing state that I could find, the tone map's inputs match normal play (above), and
  `Driver.exe` never names the grain page or its textures.
- **No usable menu call.** Lua's `Menu.ShowMenuElement(scene, element, show)` reaches the scene's
  `+0x34` (`FUN_006CAE00`), which only searches the scene's current page. For `grain_and_scratches`,
  `Replay_tint`, `Img.Replay_tint`, `base_scratch`, `black_scratch` and `Overlays` it returned 5
  (not found).

So the plugin skips the draws themselves, while the replay system is in playback (engine `+0x22C`,
mode at `+0x4E588` = 2) and only in target set 0. A draw is skipped when its stage-0 texture is one
of the five above, matched by size, format and an FNV-1a hash of mip 0's block rows (no pitch
padding, so DXVK and native D3D9 give the same hash):

| Texture | Hash |
| --- | --- |
| tint, 8x8 DXT5 | `F6604F7E` (only with the sprite pixel shader `B3A294A7`: the menu's text draws bind it too) |
| scratch, 8x800 DXT5 | `72456D1A` |
| speck, 32x32 DXT5 | `B29D6DF1` |
| hair, 64x64 DXT5 | `B7AA92E8` |
| damage atlas, 256x512 DXT1 | `1FA83DE9` |

A texture is hashed once, and only if its size and format match one of these; the cache is cleared
when playback starts and stops. Normal play never has mode 2, so its HUD is never looked at. The
switch installs the same D3D9 detours as DLSS 5 and DLAA (`InstallDlss5Hooks`), but on its own the
draw hooks pass straight through after the check: no scene or view-projection tracking.

## Film Director from code

What the main menu's Film Director "latest footage" button does (`FUN_00701260`), used by the test
build to start playback without menu keys:

1. `FUN_00701750(-1, &handle)`: the playback source, `0x00F2C5C8` = -1 (latest footage,
   `dng.replay`; 4 is `loaded.replay`, Film Director's saved films);
2. `FUN_00497A40(frontend)`, `frontend` = engine `+0x37C`: calls Lua `flagStartingReplay` and starts
   the game, which comes up in the `Replay` config with playback paused at the start.

Play/pause is `FUN_007436A0` (Space). The UI sets the speed (`+0x4E99C`) from its own "playing"
variable every frame, so writing the speed directly doesn't hold.
