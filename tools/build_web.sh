#!/bin/sh
# Builds the firmware and assembles the browser flasher site in _site/:
# web/ (page + manifest), the screenshots, and one merged image with the
# bootloader, partition table and app at their flash offsets.
# Needs PlatformIO (pio) and esptool (pip install esptool) on PATH.
set -e
cd "$(dirname "$0")/.."

pio run -e lcd-35
B=.pio/build/lcd-35
BOOT_APP0="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

rm -rf _site
mkdir -p _site
cp web/* _site/
cp docs/*.png _site/
esptool.py --chip esp32s3 merge_bin -o _site/firmware.bin \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 "$B/bootloader.bin" 0x8000 "$B/partitions.bin" 0xe000 "$BOOT_APP0" 0x10000 "$B/firmware.bin"

# Stamp the version (short commit) into the manifest so the page shows what it installs.
VERSION="${VERSION:-$(git describe --tags --always --dirty --match "v[0-9]*")}"
sed -i.bak "s/__VERSION__/$VERSION/" _site/manifest.json _site/index.html && rm _site/*.bak
echo "Site ready in _site/ (version $VERSION)"
