SUMMARY = "HC-12 device tree overlay"
DESCRIPTION = "Compiles hc12.dts into hc12.dtbo and stages it in DEPLOY_DIR_IMAGE. The wic image picks it up via IMAGE_BOOT_FILES; the RPi firmware applies it when config.txt contains `dtoverlay=hc12`. Depends on the stock uart3 overlay for UART3 enable + pinctrl."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

SRC_URI = "file://hc12.dts"
S = "${UNPACKDIR}"

DEPENDS = "dtc-native"

# Nothing to install into the rootfs — output is consumed via DEPLOYDIR
# by the wic/boot-partition pipeline.
PACKAGES = ""
do_install[noexec] = "1"

inherit deploy

do_compile() {
    dtc -@ -I dts -O dtb -o ${B}/hc12.dtbo ${S}/hc12.dts
}

do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${B}/hc12.dtbo ${DEPLOYDIR}/hc12.dtbo
}
addtask deploy after do_compile before do_build
