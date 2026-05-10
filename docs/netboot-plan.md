# Network boot — Raspberry Pi 4 (whinlatter)

> **Status: working.** SD card holds GPU firmware + U-Boot. U-Boot fetches the
> kernel via TFTP from WSL2; the rootfs is mounted from NFS on the same host.
> Iterating on a build only needs `kas build` + `./scripts/deploy-netboot.sh`
> + power-cycle the Pi. The SD is only re-flashed when overlays, U-Boot, or
> kernel binary location change.

## Phase 1 — Windows / WSL2 host

### 1.1 Enable WSL2 mirrored networking

Otherwise the Pi can't reach services running inside WSL2.

`%USERPROFILE%\.wslconfig`:
```ini
[wsl2]
networkingMode=mirrored
firewall=true
[experimental]
hostAddressLoopback=true
```
Then in PowerShell: `wsl --shutdown`, reopen WSL. Confirm with `ip a` that
`eth1` (or `eth0`) has the host's LAN IP, not `172.x` / `192.168.219.x`.

### 1.2 Open Windows Firewall

PowerShell as admin:
```powershell
New-NetFirewallRule -DisplayName "TFTP"     -Direction Inbound -Protocol UDP -LocalPort 69    -Action Allow
New-NetFirewallRule -DisplayName "NFS"      -Direction Inbound -Protocol TCP -LocalPort 2049  -Action Allow
New-NetFirewallRule -DisplayName "rpcbind"  -Direction Inbound -Protocol TCP -LocalPort 111   -Action Allow
New-NetFirewallRule -DisplayName "mountd"   -Direction Inbound -Protocol TCP -LocalPort 20048 -Action Allow
```
Pi needs all four ports for the netboot RPC chain.

### 1.3 Install + configure TFTP and NFS in WSL2

```bash
sudo apt install -y tftpd-hpa nfs-kernel-server
sudo mkdir -p /srv/tftp /srv/nfs/rpi4
sudo chown tftp:tftp /srv/tftp

# tftpd-hpa root
sudo tee -a /etc/default/tftpd-hpa >/dev/null <<'EOF'
TFTP_DIRECTORY="/srv/tftp"
TFTP_OPTIONS="--secure --create"
EOF

# NFS export
echo '/srv/nfs/rpi4 *(rw,sync,no_root_squash,no_subtree_check,fsid=0)' | sudo tee /etc/exports
```

### 1.4 Pin `rpc.mountd` to a fixed port — **critical**

Ubuntu 24.04's `nfs-mountd.service` ignores `/etc/default/nfs-kernel-server`,
so mountd binds to a random port, which never matches the firewall rule.

```bash
sudo mkdir -p /etc/systemd/system/nfs-mountd.service.d
sudo tee /etc/systemd/system/nfs-mountd.service.d/override.conf >/dev/null <<'EOF'
[Service]
ExecStart=
ExecStart=/usr/sbin/rpc.mountd --manage-gids --port 20048
EOF
sudo systemctl daemon-reload
sudo systemctl restart nfs-server tftpd-hpa
```

Verify: `rpcinfo -p localhost | grep mountd` must show port `20048`.

## Phase 2 — Yocto / kas

### 2.1 Distro config (`layers/meta-a-layer/conf/distro/a-distro.conf`)

```bitbake
IMAGE_FSTYPES = "wic.bz2 tar.gz"          # wic=SD bootloader, tar.gz=NFS rootfs

# Mini-UART pinctrl is missing in the base DT; the uart1 overlay supplies it.
RPI_KERNEL_DEVICETREE_OVERLAYS = "overlays/uart1.dtbo"
RPI_EXTRA_CONFIG = "dtoverlay=uart1"

# NFS root for cmdline.txt (kept consistent with the U-Boot script bootargs).
NFS_SERVER_IP ?= "<WSL2 host LAN IP>"
CMDLINE_ROOT_FSTYPE = ""
CMDLINE_ROOT_PARTITION = "/dev/nfs"
CMDLINE_ROOTFS = "root=/dev/nfs nfsroot=${NFS_SERVER_IP}:/srv/nfs/rpi4,vers=3,nolock,tcp ip=dhcp rw"
```

### 2.2 U-Boot boot script (`layers/meta-a-layer/recipes-bsp/rpi-u-boot-scr/`)

`rpi-u-boot-scr.bbappend` — extends the upstream sed substitution to bake
`@@NFS_SERVER_IP@@` into the script:
```bitbake
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
do_compile() {
    sed -e 's/@@KERNEL_IMAGETYPE@@/${KERNEL_IMAGETYPE}/' \
        -e 's/@@KERNEL_BOOTCMD@@/${KERNEL_BOOTCMD}/' \
        -e 's/@@BOOT_MEDIA@@/${BOOT_MEDIA}/' \
        -e 's/@@NFS_SERVER_IP@@/${NFS_SERVER_IP}/' \
        "${UNPACKDIR}/boot.cmd.in" > "${WORKDIR}/boot.cmd"
    mkimage -A ${UBOOT_ARCH} -T script -C none -n "Boot script" -d "${WORKDIR}/boot.cmd" boot.scr
}
```

`files/boot.cmd.in` — TFTP only the kernel; **use `${fdt_addr}` (firmware-merged DT, not `${fdt_addr_r}`)** so the dtoverlay actually applies:
```
setenv autoload no
dhcp
setenv serverip @@NFS_SERVER_IP@@
tftp ${kernel_addr_r} @@KERNEL_IMAGETYPE@@
setenv bootargs "earlycon console=ttyS0,115200 8250.nr_uarts=1 root=/dev/nfs nfsroot=@@NFS_SERVER_IP@@:/srv/nfs/rpi4,vers=3,nolock,tcp ip=dhcp rw rootwait"
@@KERNEL_BOOTCMD@@ ${kernel_addr_r} - ${fdt_addr}
```

## Phase 3 — Build, flash, deploy

```bash
cd <repo-root>

# 1. Build (every iteration)
uv run kas build kas.yml

# 2. Flash SD (one-time, then again only on overlay/U-Boot/kernel-format changes)
bunzip2 -kf build/tmp/deploy/images/raspberrypi4-64/core-image-minimal-raspberrypi4-64.rootfs.wic.bz2
sudo dd if=build/tmp/deploy/images/raspberrypi4-64/core-image-minimal-raspberrypi4-64.rootfs.wic of=/dev/sdX bs=4M status=progress conv=fsync

# 3. Deploy kernel + rootfs (every iteration)
./scripts/deploy-netboot.sh

# 4. Power-cycle the Pi
```

## Gotchas (in case it breaks again)

- **No kernel output past `Starting kernel ...`** → almost always `${fdt_addr_r}` instead of `${fdt_addr}` in `boot.cmd` (overlays not applied → mini-UART pinctrl missing → ttyS0 driver fails).
- **Pi hangs after `IP-Config: Complete`** → mountd port mismatch with Windows Firewall. Verify `rpcinfo -p localhost | grep mountd` says `20048`. If not, the systemd drop-in (1.4) didn't take effect.
- **No U-Boot output at all** → SD wasn't actually re-flashed, or `dtoverlay=disable-bt` is in `config.txt` (kicks the firmware/U-Boot serial off the wired UART). `dtoverlay=uart1` is the right one.
- **TFTP works but NFS times out** → mirrored networking partially active. WSL2 needs to be on `192.168.1.x`, not `192.168.219.x`. `wsl --shutdown` from PowerShell, reopen.
