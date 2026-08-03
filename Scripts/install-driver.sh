#!/bin/bash
#
# Installs the CoreAudio HAL virtual device driver.
#
# Requires sudo: the driver lives in /Library/Audio/Plug-Ins/HAL, must be
# owned by root:wheel, and coreaudiod has to be restarted to pick it up.
# Restarting coreaudiod briefly interrupts all system audio.
#
# Usage:  ./Scripts/install-driver.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE="$REPO_ROOT/HALPlugin/build/AudioMixer.driver"
DEST="/Library/Audio/Plug-Ins/HAL"

if [ ! -d "$BUNDLE" ]; then
    echo "Driver not built. Run 'make driver' first." >&2
    exit 1
fi

# An unsealed bundle signature makes coreaudiod skip the driver silently,
# with no error anywhere — worth failing loudly here instead.
if ! codesign --verify --deep "$BUNDLE" 2>/dev/null; then
    echo "Bundle signature invalid. Run 'make driver' to rebuild and re-sign." >&2
    exit 1
fi

echo "Installing AudioMixer.driver to $DEST (requires sudo)..."
sudo rm -rf "$DEST/AudioMixer.driver"
sudo cp -R "$BUNDLE" "$DEST/"
sudo chown -R root:wheel "$DEST/AudioMixer.driver"

echo "Restarting coreaudiod — system audio will glitch briefly..."
sudo killall coreaudiod

sleep 3

echo
if system_profiler SPAudioDataType 2>/dev/null | grep -q "AudioMixer"; then
    echo "SUCCESS: 'AudioMixer Output' is registered with CoreAudio."
else
    echo "Driver installed, but no device appeared. To find out why, run:"
    echo "  log stream --predicate 'eventMessage CONTAINS \"com.audiomixer.halplugin\"'"
    echo "An unimplemented property will be logged there by name."
    exit 1
fi
