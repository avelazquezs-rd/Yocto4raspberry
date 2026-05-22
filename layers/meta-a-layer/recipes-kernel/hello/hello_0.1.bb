SUMMARY = "Hello-world out-of-tree kernel module (scaffold)"
DESCRIPTION = "Skeleton out-of-tree module developed in-layer; will move to its own git repo later."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit module

SRC_URI = " \
    file://Makefile \
    file://hello.c \
"

S = "${UNPACKDIR}"

RPROVIDES:${PN} += "kernel-module-hello"
