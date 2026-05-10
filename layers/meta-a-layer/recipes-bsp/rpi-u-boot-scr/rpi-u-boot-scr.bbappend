FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Extend the upstream sed substitution with @@NFS_SERVER_IP@@ so the
# netboot boot.cmd.in (also provided by this layer) gets the NFS server's
# IP baked in at build time.
do_compile() {
    sed -e 's/@@KERNEL_IMAGETYPE@@/${KERNEL_IMAGETYPE}/' \
        -e 's/@@KERNEL_BOOTCMD@@/${KERNEL_BOOTCMD}/' \
        -e 's/@@BOOT_MEDIA@@/${BOOT_MEDIA}/' \
        -e 's/@@NFS_SERVER_IP@@/${NFS_SERVER_IP}/' \
        "${UNPACKDIR}/boot.cmd.in" > "${WORKDIR}/boot.cmd"
    mkimage -A ${UBOOT_ARCH} -T script -C none -n "Boot script" -d "${WORKDIR}/boot.cmd" boot.scr
}
