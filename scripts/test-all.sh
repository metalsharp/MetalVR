#!/bin/sh
# Runs every test MetalVR has against the simulated headset and prints one line for each.
#
#   scripts/test-all.sh
#
# The Wine cases need a Wine with Mullion (METALVR_WINE_ROOT, default MetalSharp-Wine-x86_64), and the OpenVR
# case needs build/opencomposite/openvr_api.dll; a case whose requirement is missing is reported as skipped.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
out=$root/build/out
mkdir -p "$out"
failures=0

run() { # name, command...
  name=$1
  shift
  "$@" >"$out/$name.suite.log" 2>&1
  status=$?
  if [ "$status" -eq 0 ]; then
    echo "ok    $name"
  elif [ "$status" -eq 2 ]; then
    echo "skip  $name ($(tail -1 "$out/$name.suite.log"))"
  else
    echo "FAIL  $name (see $out/$name.suite.log)"
    failures=$((failures + 1))
  fi
}

native() { # name, xr_smoke arguments...
  n=$1
  shift
  run "$n" env METALVR_TEST_NAME="$n" "$root/scripts/test-native.sh" "$@"
}
wine() {
  n=$1
  shift
  run "$n" env METALVR_TEST_NAME="$n" "$root/scripts/test-wine.sh" "$@"
}

native native --frames 150
native native-array --frames 150 --array
native native-unorm --frames 150 --unorm
native native-upright --frames 150 --no-quad
native native-flip --frames 150 --no-quad --flip
run native-headset-size env METALVR_TEST_NAME=native-headset-size \
  METALVR_DAEMON_ARGS="--eye 2144x2336 --codec hevc" "$root/scripts/test-native.sh" --frames 300
run failures "$root/scripts/test-failures.sh"
# the headsets (docs/HEADSETS.md): each controller kind's interaction profile, the protocols of the headsets MetalVR
# drives itself, and each one's panel drawn through its lens model
run native-vive-wands env METALVR_TEST_NAME=native-vive-wands METALVR_DAEMON_ARGS="--headset vive" \
  "$root/scripts/test-native.sh" --frames 120 --expect-profile /interaction_profiles/htc/vive_controller
run native-index-touch env METALVR_TEST_NAME=native-index-touch METALVR_DAEMON_ARGS="--headset index" \
  "$root/scripts/test-native.sh" --frames 120
run native-psvr2-sense env METALVR_TEST_NAME=native-psvr2-sense METALVR_DAEMON_ARGS="--headset psvr2" \
  "$root/scripts/test-native.sh" --frames 120 --expect-profile /interaction_profiles/oculus/touch_controller
run headset-protocols "$root/build/native/tests/headset_protocol_check"
run foveation "$root/build/native/daemon/metalvrd" --foveation-check
run metalfx-upscale "$root/build/native/daemon/metalvrd" --upscale-check
run libsurvive "$root/build/native/daemon/metalvrd" --survive-check sim
if [ -f "$root/build/basalt/prefix/lib/libbasalt.dylib" ] && [ -x "$root/build/native/tests/rift_s_tracking_check" ]; then
  run rift-s-tracking "$root/build/native/tests/rift_s_tracking_check" 24
else
  echo "skip  rift-s-tracking (Basalt is not built; scripts/build-basalt.sh)"
fi
run passthrough "$root/build/native/daemon/metalvrd" --passthrough-check
run native-foveated env METALVR_TEST_NAME=native-foveated METALVR_DAEMON_ARGS="--foveated-encoding --codec hevc" \
  "$root/scripts/test-native.sh" --frames 120
for model in rift-cv1 rift-s index vive vive-pro vive-pro-2 psvr2; do
  run "panel-$model" "$root/build/native/daemon/metalvrd" --panel-check "$model" "$out/panel-$model.png"
done
if [ -x "$root/build/native/tests/alvr_driver_smoke" ]; then
  run alvr-models "$root/scripts/test-alvr-models.sh"
else
  echo "skip  alvr-models (metalvrd was built without ALVR; scripts/build-alvr.sh)"
fi
if [ -x "$root/build/native/tests/alvr_driver_smoke" ]; then
  rm -rf "$out/alvr-smoke"
  run alvr-driver "$root/build/native/tests/alvr_driver_smoke" "$out/alvr-smoke" 2 1234.client
else
  echo "skip  alvr-driver (metalvrd was built without ALVR; scripts/build-alvr.sh)"
fi

wine wine --frames 150
wine wine-array-msaa --frames 150 --array --samples 4
wine wine-unorm --frames 150 --unorm
wine wine-upright --frames 150 --no-quad
wine wine-flip --frames 150 --no-quad --flip
# every Direct3D format the runtime offers: BGRA sRGB and UNORM, RGBA16F, RGB10A2, RGBA16, BGRX sRGB and UNORM
for format in 91 87 10 24 11 93 88; do
  wine "wine-format-$format" --frames 90 --format "$format"
done
run wine-headset-size env METALVR_TEST_NAME=wine-headset-size \
  METALVR_DAEMON_ARGS="--eye 2144x2336 --codec hevc" "$root/scripts/test-wine.sh" --frames 300 --array
if [ -f "$root/build/openxr-win64/src/loader/libopenxr_loader.dll" ]; then
  wine_binary=${METALVR_WINE_ROOT:-$HOME/.metalsharp/runtime/MetalSharp-Wine-x86_64.app/Contents}/MacOS/wine
  "$root/scripts/install-prefix.sh" "$wine_binary" "${METALVR_WINEPREFIX:-$root/build/prefix}" >/dev/null 2>&1
  run wine-loader env METALVR_TEST_NAME=wine-loader \
    METALVR_LOADER="$root/build/openxr-win64/src/loader/libopenxr_loader.dll" "$root/scripts/test-wine.sh" --frames 120
else
  echo "skip  wine-loader (no Khronos loader built for Windows in build/openxr-win64)"
fi
run openvr "$root/scripts/test-openvr.sh"

[ "$failures" -eq 0 ] && echo "everything passed" || echo "$failures suite(s) failed"
exit "$failures"
