SUMMARY = "CYW43439 WiFi firmware"
LICENSE = "CLOSED"

SRC_URI = "file://brcmfmac43439-sdio.bin \
           file://brcmfmac43439-sdio.txt \
           file://brcmfmac43439-sdio.clm_blob \
           file://brcmfmac43439.hcd"

S = "${WORKDIR}"

do_install() {
    install -d ${D}${nonarch_base_libdir}/firmware/brcm

    install -m 0644 brcmfmac43439-sdio.bin ${D}${nonarch_base_libdir}/firmware/brcm/
    install -m 0644 brcmfmac43439-sdio.txt ${D}${nonarch_base_libdir}/firmware/brcm/
    install -m 0644 brcmfmac43439-sdio.clm_blob ${D}${nonarch_base_libdir}/firmware/brcm/
    install -m 0644 brcmfmac43439.hcd ${D}${nonarch_base_libdir}/firmware/brcm/

    # Create required symlink
    ln -sf brcmfmac43439-sdio.bin \
        ${D}${nonarch_base_libdir}/firmware/brcm/brcmfmac43439-sdio.compulab,ucm-imx8m-plus.bin
}

FILES:${PN} += "${nonarch_base_libdir}/firmware/brcm/*"
