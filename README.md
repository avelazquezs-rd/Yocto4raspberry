# Yocto build environment

kas-driven Yocto build for Raspberry Pi 4 (64-bit), targeting the
`whinlatter` release (Yocto 5.3.3). Whinlatter is the first release
where the bundled `poky` repository is no longer used — the Poky
reference distribution is now assembled from three upstream repos:
`bitbake`, `openembedded-core`, and `meta-yocto`. kas clones all four
(plus `meta-raspberrypi`) into `layers/`.

Builds the stock `core-image-minimal` with `local.conf` tweaks for a
serial console on UART and a single bootloader / kernel / DTB.

## Prerequisites

- `uv` on PATH (install: `curl -LsSf https://astral.sh/uv/install.sh | sh`)
- A Linux host with the standard Yocto build dependencies
  (`build-essential`, `chrpath`, `diffstat`, `file`, `gawk`, `wget`,
  `python3`, `git`, `cpio`, `unzip`, `zstd`, …)

## Running kas

All commands are run from the repository root and invoked through
`uv run`, which keeps `.venv` in sync with `pyproject.toml` automatically.

The build needs the LAN IP of the host that will serve TFTP + NFS to the
Pi (see `docs/netboot-plan.md`). Export it before running kas:

```bash
export NFS_SERVER_IP=<host-LAN-IP>    # the build host's LAN IP

# First-time setup: clone the upstream layers into ./layers/
# and generate bblayers.conf + local.conf in ./build/conf/.
uv run kas checkout kas.yml

# Full build of the target declared in kas.yml (core-image-minimal).
uv run kas build kas.yml

# One-off bitbake command in the kas-prepared environment.
uv run kas shell kas.yml -c 'bitbake core-image-minimal'

# Show the resolved configuration (debug aid).
uv run kas dump kas.yml
```

## Layout after `kas checkout`

```
<repo-root>/
  pyproject.toml          # declares kas dependency for uv
  uv.lock                 # pinned uv resolution
  kas.yml                 # single source of truth (machine, distro, layers, env)
  scripts/
    deploy-netboot.sh     # copies kernel to /srv/tftp + rootfs to /srv/nfs/rpi4
  layers/
    bitbake/              # build engine                  (yocto-5.3.3)  [kas-managed]
    openembedded-core/    # provides meta/                (yocto-5.3.3)  [kas-managed]
    meta-yocto/           # meta-poky + meta-yocto-bsp    (yocto-5.3.3)  [kas-managed]
    meta-raspberrypi/     # BSP                           (whinlatter)   [kas-managed]
    meta-a-layer/         # this project's custom layer + distro
  build/
    conf/
      bblayers.conf       # generated from kas.yml — DO NOT hand-edit
      local.conf          # generated from kas.yml — DO NOT hand-edit
    tmp/, downloads/, sstate-cache/
```

## Flashing / netboot

After `uv run kas build kas.yml`:

```
build/tmp/deploy/images/raspberrypi4-64/core-image-minimal-raspberrypi4-64.rootfs.wic.bz2
```

Decompress and `dd` to your SD card. For network-boot setup (TFTP kernel
+ NFS rootfs), see [docs/netboot-plan.md](docs/netboot-plan.md).
