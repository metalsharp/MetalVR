# MetalVR in MetalSharp-Wine

MetalVR is its own project; this is what MetalSharp-Wine needs to carry it. Nothing here requires a Wine or
Mullion patch.

## What ships

| File | Where | Notes |
|---|---|---|
| `metalvr_openxr.dll`, `metalvr_openxr.json` | a directory of the bundle, e.g. `share/metalsharp/metalvr/x86_64-windows/` | one self-contained x86_64 DLL; imports only `KERNEL32`, `WS2_32`, `dxgi` and the UCRT |
| `metalvrd` | the bundle's `MacOS/` or `libexec/` | native arm64; links ALVR's `server_core` when built with it |
| `openvr_api.dll` (OpenComposite) | beside the above, as its own GPLv3 component | only for SteamVR games; see [OPENCOMPOSITE.md](OPENCOMPOSITE.md) |

## Per prefix

A game's OpenXR loader finds the runtime through the registry:

```
HKLM\SOFTWARE\Khronos\OpenXR\1
    ActiveRuntime = C:\Program Files\MetalVR\metalvr_openxr.json   (REG_SZ)
```

`scripts/install-prefix.sh WINE PREFIX` copies the DLL and manifest into the prefix and sets the value;
`--remove` undoes it. In MetalSharp this belongs where a prefix's other runtime components are set up.

## Per launch

1. `metalvrd` must be running before the game creates its OpenXR instance, started with `--trust` and the name
   the headset's ALVR client shows; a headset that is not named is refused ([ALVR.md](ALVR.md)). Without it the runtime returns
   `XR_ERROR_RUNTIME_UNAVAILABLE` and a game with a flat mode starts flat. One `metalvrd` serves one headset and
   one session at a time.
2. The game must be on the Mullion lane. On MetalSharp-Wine x86_64 as shipped that is `GRAPHICS_BACKEND=dxmt`
   in the game's environment or `launch.env` (the runtime this was tested on routed Direct3D to Mullion for
   `dxmt` and not for `mullion`). On wined3d, DXVK or D3DMetal the runtime's swapchains cannot be
   shared and `xrCreateSwapchain` fails with a log line saying that Mullion is needed.
3. The runtime reads these from the game's environment:

   | Variable | Effect |
   |---|---|
   | `METALVR_PORT` | the loopback port, for both sides (default 37913) |
   | `METALVR_LOG=1` | the runtime logs to the game's stderr |
   | `METALVR_HEADSET_WAIT=<seconds>` | `xrGetSystem` waits that long for a headset to connect before answering that there is none; for games that treat a missing headset as fatal (every OpenVR game through OpenComposite) |
   | `METALVR_INTERACTION_PROFILE=<path>` | the interaction profile to prefer when a game suggests bindings for several, e.g. `/interaction_profiles/htc/vive_controller` for a game whose Vive bindings are the good ones |
   | `METALVR_METALFX_SPATIAL=<factor>` | render each eye smaller by this factor (`1.5`, `2`; at most 4) for `metalvrd` to scale up with MetalFX; `on` uses Mullion's factor, or 2 if that is 1; `0` or `off` turns it off whatever Mullion's config says. Unset, MetalVR follows MetalSharp's MetalFX toggle (below) |

## MetalFX upscaling in VR

MetalSharp's MetalFX Spatial toggle writes `d3d11.metalSpatialUpscale` and `d3d11.metalSpatialUpscaleFactor` to the
Mullion config file that `MULLION_CONFIG_FILE` names. Mullion applies them only to a DXGI swapchain, and a VR
game's eyes never pass through one: they go to MetalVR's swapchains (OpenXR) or its SteamVR driver's textures, and
Mullion's upscaling reaches only the game's desktop mirror window.

So MetalVR reads the same settings itself (`runtime/upscale.cpp`) from the game's environment, as Mullion does
(`Config::getUserConfig`, `env::getEnvVar`):

- the file `MULLION_CONFIG_FILE` names, or `mullion.conf` (then `dxmt.conf`) in the game's working directory, then
  the lines in `MULLION_CONFIG`, with Mullion's `[game.exe]` sections; each `MULLION_` variable also answers to its
  `DXMT_` name;
- on with `d3d11.metalSpatialUpscale = True` or `MULLION_METALFX_SPATIAL_SWAPCHAIN=1`, at
  `d3d11.metalSpatialUpscaleFactor` (2 when it is missing or not a number Mullion reads; MetalVR stops at 4).

With it on and a factor above 1:

- the OpenXR runtime recommends each eye at the headset's size divided by the factor
  (`xrEnumerateViewConfigurationViews`; the largest size a game may ask for is unchanged), and the SteamVR
  driver's recommended render target size is divided the same way;
- `metalvrd` scales the game's eyes (the bottom projection layer) up to the headset's with MetalFX's spatial
  scaler before composing when they are smaller (`daemon/compositor.mm`), and logs `compositor: MetalFX spatial
  upscaling WxH to WxH per eye` for the first sizes it sees. One scaler serves each eye whatever size the game
  renders at, so dynamic resolution costs nothing more. Layers blended over the game's eyes (SteamVR's dashboard
  and overlays), quad layers and foveated encoding are drawn as before.

The scaling is done while the game waits for its frame to be taken (about 1 ms per eye at a Quest 3's size, on
the Mac it was measured on), so it pays only in a game whose frame rate is held back by the size it renders at:
BONELAB went from about 28 to 60 frames a second at factor 2, while Blade & Sorcery, which already held 72 at
full size, had nothing to gain and ran slower.

A factor of 1 (MetalSharp's default) renders at the headset's size, so the toggle changes nothing in VR until
the factor is raised; the runtime (with `METALVR_LOG=1`) and the SteamVR driver say so in their logs. `METALVR_METALFX_SPATIAL` in the game's environment overrides the config. `metalvrd` reads
the same variable from its own environment, where only `0` (`off`) matters: smaller eyes are then drawn with a
bilinear sample, as before. A game that picks its own render size, or SteamVR's resolution setting, still decides
how large the eyes are; whatever arrives smaller than the headset's eye is scaled with MetalFX either way.

## The Mullion contract

The runtime and server rely on Mullion's Direct3D 11 shared textures
(`D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE`):

- the NT handle from `IDXGIResource1::CreateSharedHandle` is a file mapping that contains the texture's bootstrap
  name, `MULLION_shared_` and 32 hexadecimal digits (`runtime/backend_d3d11.cpp`, `mullionShareName`);
- a bare mach message with id `0x4d4c534e` to that name is answered, with id `0x4d4c534e + 100`, by a send right
  to the texture's Metal shared-texture port (`daemon/shared_texture.mm`);
- `D3D11_QUERY_EVENT` completes only when the GPU has executed what was issued before it.

All three hold for Mullion's source at `leap-metalsharp` and for the builds in the two installed MetalSharp-Wine
runtimes listed below. They are internal to Mullion, so a change there breaks MetalVR at swapchain creation (loudly, not silently). Two things would turn
this into a real contract, and both are small:

- a private interface on Mullion's D3D11 texture that returns the bootstrap name (or a duplicate of the port
  name), so the runtime stops reading the handle's mapping;
- a Mullion test that opens a shared texture from a process that is not Mullion, the way `metalvrd` does.

Direct3D 12 games need the same from Mullion's D3D12 (`ID3D12Device::CreateSharedHandle` on a committed
resource) and an `XR_KHR_D3D12_enable` backend in the runtime; neither exists yet.

## What was verified, and on what

On macOS 27.3, Apple silicon:

| Runtime | Result |
|---|---|
| `MetalSharp-Wine-x86_64.app` (wine-11.17 under Rosetta, Mullion 39d1e4a4 in `lib/dxmt`) | `scripts/test-wine.sh` passes: per-eye swapchains, an array swapchain, 4x MSAA, both together, a non-sRGB format; also through the Khronos loader and the prefix's `ActiveRuntime` (`METALVR_LOADER=...`) |
| `MetalSharp-Wine.app` (arm64 with FEX, Mullion in `lib/mullion`) | with the same x86_64 runtime DLL and test programs: `scripts/test-wine.sh` passes (per-eye swapchains with a quad layer, array with 4x MSAA, a mirrored image, a headset-sized HEVC stream) and `scripts/test-openvr.sh` passes |

In each case a Direct3D 11 application ran a full session against `metalvrd --driver sim`; the encoded stream
held every frame and decoded, and the composed frame was the picture the application drew. On the x86_64
runtime that includes every swapchain format the runtime offers and the OpenVR path through OpenComposite
(`scripts/test-all.sh`).

Not verified: any real game; any real headset.
