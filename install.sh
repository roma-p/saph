#!/bin/sh
# Installs build/MillSaph.ofx.bundle into the OFX plugin folder: /Library/OFX/Plugins on
# macOS, /usr/OFX/Plugins on Linux.
set -e
cd "$(dirname "$0")"

bundle=build/MillSaph.ofx.bundle
case "$(uname)" in
Darwin) dest=/Library/OFX/Plugins ;;
*)      dest=/usr/OFX/Plugins ;;
esac

[ -d "$bundle" ] || { echo "$bundle not found: run ./build.sh first"; exit 1; }

sudo mkdir -p "$dest"
sudo rm -rf "$dest/MillSaph.ofx.bundle"
sudo cp -R "$bundle" "$dest/"
sudo chmod -R a+rX "$dest/MillSaph.ofx.bundle"
echo "installed $dest/MillSaph.ofx.bundle - restart your DCC."
