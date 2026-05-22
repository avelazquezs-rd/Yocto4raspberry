#!/usr/bin/env bash
# Deploy the latest core-image-minimal artifacts into the WSL2-side
# TFTP and NFS roots so the Pi can pick them up on the next power-cycle.
#
# Run from the repository root after `uv run kas build kas.yml`.
#
# Prerequisites — one-time host setup (see docs/netboot-plan.md §1.3):
#   * The invoking user has write access to /srv/tftp and /srv/nfs/rpi4
#     (e.g. via group membership + group-writable mode on those dirs).
#   * /etc/exports uses `all_squash,anonuid=0,anongid=0` for /srv/nfs/rpi4
#     so the NFS server presents every file as root:root over the wire
#     regardless of on-disk ownership. Without this, extracting the rootfs
#     as a non-root user produces a broken rootfs.

set -euo pipefail

DEPLOY=build/tmp/deploy/images/raspberrypi4-64
TFTP=/srv/tftp
NFS=/srv/nfs/rpi4
ROOTFS_TAR="${DEPLOY}/image-minimal-ext-raspberrypi4-64.rootfs.tar.gz"
KERNEL="${DEPLOY}/Image"

for f in "$ROOTFS_TAR" "$KERNEL"; do
    [[ -e "$f" ]] || { echo "Missing artifact: $f" >&2; exit 1; }
done

for d in "$TFTP" "$NFS"; do
    [[ -w "$d" ]] || {
        echo "Not writable as $USER: $d" >&2
        echo "Run the one-time host setup at the top of this script." >&2
        exit 1
    }
done

install -m 0644 "$KERNEL" "$TFTP/Image"

rm -rf "$NFS"/*
# --strip-components=1 drops the leading "./" component from each entry
# AND silently skips the bare "./" root entry, so tar never tries to
# chmod/utime $NFS itself (which we don't own).
tar --strip-components=1 -xzf "$ROOTFS_TAR" -C "$NFS"

echo "Deployed:"
echo "  TFTP : $TFTP/Image"
echo "  NFS  : $NFS/  ($(du -sh "$NFS" | cut -f1))"
