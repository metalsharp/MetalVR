# SteamVR

MetalVR's SteamVR driver makes the headset `metalvrd` drives one of SteamVR's own. Windows SteamVR, running under
MetalSharp's Wine, lists it as a headset (an Oculus Rift CV1 shows as "Oculus Rift CV1"), and SteamVR games and
SteamVR Home render into it. OpenXR games don't need SteamVR: they use MetalVR's OpenXR runtime directly.

## How it works

SteamVR loads `driver_metalvr.dll` into its server process, `vrserver.exe`. The driver:

- connects to `metalvrd` on the loopback port, as the OpenXR runtime does, and registers one HMD with the
  headset's name, refresh rate, per-eye render size and field of view. With MetalFX upscaling on, the render size
  it recommends is the headset's divided by the factor and `metalvrd` scales the eyes back up; the driver reads the
  setting from `vrserver.exe`'s environment, where a `[game.exe]` section of Mullion's config does not apply
  ([METALSHARP_WINE.md](METALSHARP_WINE.md#metalfx-upscaling-in-vr));
- sends SteamVR the head's pose about 500 times a second;
- takes SteamVR's frames in **direct mode**. SteamVR's compositor renders each eye into textures the driver made.
  These are Mullion shared textures, so `metalvrd` opens the same Metal textures and shows each frame with its own
  lens correction and reprojection. SteamVR's own distortion is switched off (the identity).

With a Rift's sensors (RIFT.md) the driver also registers each sensor as a tracking reference, which SteamVR shows as
a base station where the sensor stands, once the sensor has seen the headset.

Each `Present` from the compositor waits for SteamVR's sync texture, then waits on a Direct3D 11 fence until the
compositor's drawing into the eye images is done (up to 100 ms), and only then sends the frame to `metalvrd` as one
projection layer. The sync texture's keyed mutex orders only the driver's own GPU queue, not `metalvrd`'s: without
the fence, `metalvrd` could read half-drawn eye images, which show as flashing magenta, red, green or blue blocks and
lines. The compositor's next frame starts at the headset's next refresh, as `metalvrd` paces it.

## Setting it up

1. Install MetalVR (`install.sh`). It copies the driver folder into
   `~/.metalsharp/metalvr-runtime/lib/metalvr/steamvr/metalvr`.
2. Start SteamVR once, so the prefix has an `openvrpaths.vrpath`, then quit it.
3. Register the driver with the Steam prefix's SteamVR:

   ```
   ~/.metalsharp/metalvr-runtime/bin/metalvr steamvr
   ```

   `metalvr steamvr --off` takes it out again. The file as it was before the first change is kept beside it as
   `openvrpaths.vrpath.metalvr-orig`. The same command keeps the Steam client itself out of VR (below); restart
   Steam after it.
4. Start `metalvrd` (for a headset on the Mac's own display, its script: `metalvr-rift start`,
   `metalvr-lighthouse start` or `metalvr-psvr2 start`; see its guide in [headsets/](headsets/README.md)), then
   start SteamVR.

SteamVR needs `metalvrd` running when it starts. Without it the driver reports no headset, and SteamVR says so.

## Requirements and limits

- SteamVR, and the driver inside it, must run on Mullion's Direct3D 11 (MetalSharp's `dxmt` pipeline): the
  driver's textures are Mullion shared textures.
- Only one application can show on the headset at a time. While SteamVR's compositor holds `metalvrd`'s session,
  an OpenXR game started outside SteamVR waits; a game started from SteamVR goes through SteamVR.
- SteamVR's compositor, the driver and the applications share Direct3D resources between processes, which Mullion
  does only for a VR runtime: `MULLION_METALVR=1` has to be in the environment SteamVR and its applications start
  with. `metalvr steamvr` sets it in the Steam prefix's user environment (`HKCU\Environment`), which Wine gives every
  Windows process it starts in the prefix, however Steam was started; `metalvr steamvr --off` takes it out. Mullion
  has this from aaf2tbz/mullion `998c32a6` (MetalSharp-Wine's `src/mullion`). Without it an application runs but is
  black in the headset, and logs `Failed to open frame info buffer (D3D11)!`.
- The Steam client must not join SteamVR as a VR application of its own. It does so through its `openvr_api.dll`,
  to draw itself in the headset, and under Wine that stalls its main loop for good a few seconds after SteamVR
  starts: Steam's windows load forever, and SteamVR, which asks Steam before it loads an application's controller
  bindings, gets no answer, so applications get no controller input. `metalvr steamvr` keeps `steam.exe` alone from
  loading that DLL (a per-application Wine override; games use their own copy). The cost: Steam does not show itself
  in the headset, and writes an assert dump about the DLL when it starts.
- The controllers are registered as the ones in the user's hands, which `metalvrd` names (`metalvr_headsets.h`,
  [HEADSETS.md](HEADSETS.md)): SteamVR's own Touch profile with the CV1's, Rift S's, Quest 2's or Quest 3's models;
  its Index controller profile, with each finger's curl and the hand skeleton; or its Vive controller profile.
  The Steam Frame's controllers are shown as ringless Touch controllers until SteamVR's own names for them are known,
  and the PlayStation VR2's Sense controllers as Touch controllers named for the Sense.
  Which ones they are is read when SteamVR starts the driver: a headset that connects later with other controllers
  needs SteamVR started again. Battery levels show where the headset reports them (ALVR's headsets).
- With the Rift's sensors the controllers and the headset are tracked in position; without them, and on the other
  headsets MetalVR drives itself, the headset is tracked in rotation only, at a fixed seated height, and the
  controllers are placed beside the body.
- SteamVR covers the whole view with a fade color when it judges the application's frames or the tracking not good
  enough, and with MetalVR it never lifts that fade: the headset shows one flat color (teal). `metalvr steamvr`
  turns both fades off (`steamvr.doNotFadeToGrid`, `steamvr.forceFadeOnBadTracking`).
- MetalSharp starts a game on its MetalVR route through the game's VR launch entry (`steam://launch/<id>/VR`). A
  game without VR support still starts flat: Subnautica: Below Zero, for one, ships no OpenVR runtime.

## When it doesn't work

SteamVR writes the driver's messages, each starting with `metalvr:`, to `vrserver.txt` in Steam's `logs` folder
inside the prefix (`drive_c/Program Files (x86)/Steam/logs`).

| Message | Meaning |
| --- | --- |
| `metalvrd is not running` | Start `metalvrd` first, then SteamVR. |
| `D3D11CreateDevice failed` / `shared texture … failed` | SteamVR isn't running on Mullion's Direct3D 11. |
| `metalvrd's session is held by another application` | An OpenXR game is showing on the headset; close it. |
| `format N is not one the headset can show` | The compositor asked for a texture format the server can't open. |
