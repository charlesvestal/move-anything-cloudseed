#!/bin/bash
# Install CloudSeed module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/cloudseed" ]; then
    echo "Error: dist/cloudseed not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing CloudSeed Module ==="

# Deploy to Move - audio_fx subdirectory
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/move-anything/modules/audio_fx/cloudseed"
scp -r dist/cloudseed/* ableton@move.local:/data/UserData/move-anything/modules/audio_fx/cloudseed/

# Install chain presets if they exist
if [ -d "src/chain_patches" ]; then
    echo "Installing chain presets..."
    ssh ableton@move.local "mkdir -p /data/UserData/move-anything/patches"
    scp src/chain_patches/*.json ableton@move.local:/data/UserData/move-anything/patches/
fi

# Set permissions so Module Store can update later
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/move-anything/modules/audio_fx/cloudseed"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/move-anything/modules/audio_fx/cloudseed/"
echo "Presets installed to: /data/UserData/move-anything/patches/"
echo ""
echo "Restart Move Anything to load the new module."
