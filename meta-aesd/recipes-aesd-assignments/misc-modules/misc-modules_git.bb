SUMMARY = "Misc-modules kernel modules"
DESCRIPTION = "${SUMMARY}"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

FILESEXTRAPATHS:prepend := "${THISDIR}/file:"
SRC_URI = "git://github.com/cu-ecen-aeld/assignment-7-sbrhss.git;protocol=https;branch=main"
SRC_URI += "file://misc-modules-init"

# Modify these as desired
PV = "1.0+git${SRCPV}"
SRCREV = "b9a8acb9506044ada7e88f1f562f695ccb9d59c8"

S = "${WORKDIR}/git/misc-modules"

inherit module

EXTRA_OEMAKE:append:task-install = " -C ${STAGING_KERNEL_DIR} M=${S}/misc-modules"
EXTRA_OEMAKE += "KERNELDIR=${STAGING_KERNEL_DIR}"

inherit update-rc.d
INITSCRIPT_PACKAGES = "${PN}"
INITSCRIPT_NAME:${PN} = "misc-modules-init"

FILES:${PN} += "${sysconfdir}/*"

do_compile () {
        oe_runmake
}

do_install () {
        install -d ${D}${sysconfdir}/init.d
        install -d ${D}${base_libdir}/modules/${KERNEL_VERSION}/
        install -m 0755 ${WORKDIR}/misc-modules-init ${D}${sysconfdir}/init.d
        install -m 0755 ${S}/hello.ko ${D}${base_libdir}/modules/${KERNEL_VERSION}/
        install -m 0755 ${S}/aesd-faulty.ko ${D}${base_libdir}/modules/${KERNEL_VERSION}/
}
