#!/bin/sh
# Installs build/MillSaph.ofx.bundle into /Library/OFX/Plugins
set -e
cd "$(dirname "$0")"

bundle=build/MillSaph.ofx.bundle
dest=/Library/OFX/Plugins

[ -d "$bundle" ] || { echo "$bundle not found: run ./build.sh first"; exit 1; }

sudo mkdir -p "$dest"
sudo rm -rf "$dest/MillSaph.ofx.bundle"
sudo cp -R "$bundle" "$dest/"
echo "installed $dest/MillSaph.ofx.bundle - restart your DCC first."
