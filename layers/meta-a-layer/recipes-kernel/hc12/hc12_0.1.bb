SUMMARY = "HC-12 433 MHz wireless UART serdev driver"
DESCRIPTION = "Out-of-tree kernel module driving the HC-12 RF UART module via a serdev port and a SET GPIO. Exposes /dev/hc12 for transparent data and sysfs (channel/baud/mode/power/version) for AT-command configuration."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit module

SRC_URI = " \
    file://Makefile \
    file://hc12.c \
"

S = "${UNPACKDIR}"

KERNEL_MODULE_AUTOLOAD += "hc12"

RPROVIDES:${PN} += "kernel-module-hc12"
