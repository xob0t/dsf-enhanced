# Passengers in the in-car camera

Driver.exe 1.04. Fixed in `src/dsf_enhanced.cpp` (`ApplyCockpitPassengersPatch`,
`ProxyOccupantPlayerVehicleCheck`), `[Quality] CockpitPassengers` (default 1).

## Symptom

After a Shift into a traffic car that carries passengers, the passengers show in the chase
camera but not in the in-car camera (`DriverEye`). The driver's seat is fine: Tanner's cockpit
arms stand in for him there.

## Prior work

No mod, forum post or cheat table found that changes this (web searches for Driver: San
Francisco passengers or occupants in the cockpit, interior or first-person view, Driver
Madness, Reddit, Nexus Mods, ModDB). The occupant code below was first mapped in the
driversf-forza project (`research/cockpit.md`, "Hands on Forza's rim" and "Hands in the chase
views"), which noted that FUN_00568590 skips the player's own driver in camera modes 3 and 4.

## Cause

Vehicle occupants come from a pool in the character manager (`[[0x00FE7228]+0x2A8] + 0x8B540`).
FUN_00567E60 sets one up: +0x360 the vehicle, +0x364 the seat (0 the driver). FUN_00568590 draws
one and starts with:

```text
00568630  call 0x7A7650             ; the local player's vehicle
00568635  mov  edx, [eax]
00568637  cmp  edx, [esi+0x360]     ; this occupant's vehicle
0056863D  jne  0x568687             ; another car: draw
0056863F  ...  call 0x5AB6D0        ; camera mode
0056866C  cmp  eax, 4 / je 0x568C33 ; return
0056867E  cmp  eax, 3 / je 0x568C33 ; return
00568687  ...                       ; draw
```

There is no seat check, so in camera modes 3 and 4 every occupant of the player's car is
skipped, passengers included. The driver has to be skipped: the cockpit arms (vtable
0x00DE3D38, drawn from the vehicle's draw, FUN_0087A5D0) replace him in that view.

## Fix

The 8 bytes at 0x00568637 (`3B 96 60 03 00 00 75 48`) become a jump to
`ProxyOccupantPlayerVehicleCheck`:

```text
cmp edx, [esi+0x360]
jne draw                    ; 0x568687
cmp dword [esi+0x364], 0
jne draw                    ; a passenger: draw
jmp 0x56863F                ; the driver: the stock camera-mode check
```

If the bytes differ, nothing is patched and the log says so. The passengers then go through
the same draw as in the chase camera, so the cost is that of the one to three extra characters
the chase camera already draws.

## Camera modes

A diagnostic build recorded the value FUN_005AB6D0 returns at 0x0056866C: 4 in `DriverEye`, 1
in the chase camera, 0 while a Lua debug camera (`CameraSystem.EnableLuaDebugCameras`) is the
viewport. So a debug camera placed in the car, even with `cameraMode` still `DriverEye`, draws
every occupant with or without the patch; screenshots for this fix have to come from the
game's own in-car camera.

## Tests (VM, stock vehicle archive, the user's ini)

Lua probe (driversf-forza harness, `-LuaFiles dsfforza\clonetest.lua=...`): picks a traffic
car whose seat 1, 2 or 3 holds an occupant (`GameVehicleResource.getCharacterSpoolingEntityIndex`
is not 65535), Shifts into it (`ZapIntoVehicle`), shows the chase camera, then `DriverEye`.

- The diagnostic build also flipped the patch every 6 s and counted the stub's passes. In
  `DriverEye` (mode 4), with the patch on, the stub saw the driver and the passenger once a
  frame each, and only the driver reached the camera-mode check; with it off, both reached it
  and returned there.
- Screenshots (`img/cockpit-passengers-1.jpg`, `-2.jpg`; runs `vm1-20261028-224517`): two
  Camaros with a front passenger, `DriverEye` turned right with the look-right key (D, from
  `media/defaultInputControlSet1.txt`) and `FOVScalar=0.03` so the passenger seat is in frame.
  Left: the patch off, the seat is empty. Right: the patch on, a few seconds later, the
  passenger is there. The forward view at the stock FOV doesn't reach the passenger seat.
- Frame time, same car and view, patch flipped every 6 s, PresentMon over 40 s, frames within
  1 s of a flip dropped: median 42.9 ms with the patch (290 frames), 43.8 ms without (254).
  The VM's frame-to-frame noise is larger than any difference.
- No crash in any run with the patch. One run at an extreme `FOVScalar` (0.045) hung on
  entering `DriverEye` while the patch was off; 0.03 ran fine.
