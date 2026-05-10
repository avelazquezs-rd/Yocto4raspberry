#!/usr/bin/env bash
# Deploy the latest core-image-minimal artifacts into the WSL2-side
# TFTP and NFS roots so the Pi can pick them up on the next power-cycle.
#
# Run from the repository root after `uv run kas build kas.yml`.

set -euo pipefail

DEPLOY=build/tmp/deploy/images/raspberrypi4-64
TFTP=/srv/tftp
NFS=/srv/nfs/rpi4
ROOTFS_TAR="${DEPLOY}/core-image-minimal-raspberrypi4-64.rootfs.tar.gz"
KERNEL="${DEPLOY}/Image"
# Note: the DTB is NOT TFTP'd. The firmware on the SD card already loads
# bcm2711-rpi-4-b.dtb and applies overlays from /overlays/ on the SD's
# boot partition; U-Boot uses that merged DT (${fdt_addr}). Re-flashing
# the SD is only needed when overlays or config.txt change.

for f in "$ROOTFS_TAR" "$KERNEL"; do
    [[ -e "$f" ]] || { echo "Missing artifact: $f" >&2; exit 1; }
done

sudo install -m 0644 "$KERNEL" "$TFTP/Image"

sudo rm -rf "$NFS"/*
sudo tar -xzf "$ROOTFS_TAR" -C "$NFS"

sudo exportfs -ra

echo "Deployed:"
echo "  TFTP : $TFTP/Image"
echo "  NFS  : $NFS/  ($(du -sh "$NFS" | cut -f1))"
