SUMMARY = "Minimal RPi image with serial console only"
LICENSE = "MIT"

IMAGE_INSTALL = "packagegroup-core-boot"
IMAGE_LINGUAS = ""
IMAGE_FEATURES = ""

IMAGE_FSTYPES = "wic.bz2"

inherit core-image
