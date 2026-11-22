if(EXISTS "${CURRENT_INSTALLED_DIR}/include/openssl/ssl.h")
    message(FATAL_ERROR "Can't build libressl if openssl is installed. Please remove openssl, and try install libressl again if you need it.")
endif()

vcpkg_download_distfile(
    LIBRESSL_SOURCE_ARCHIVE
        URLS "https://github.com/libressl/portable/releases/download/v${VERSION}/${PORT}-${VERSION}.tar.gz"
             "https://cdn.openbsd.org/pub/OpenBSD/LibreSSL/${PORT}-${VERSION}.tar.gz"
    FILENAME "${PORT}-${VERSION}.tar.gz"
    SKIP_SHA512
)

# OpenBSD publishes the SHA-256 digest for its portable release tarballs.
file(SHA256 "${LIBRESSL_SOURCE_ARCHIVE}" archive_hash)
if(NOT archive_hash STREQUAL "290cdf1b53c086255dc302f118b98602a444e9ae6d11e9bfdd8c9b1b60aa3a22")
    message(FATAL_ERROR "LibreSSL archive checksum mismatch")
endif()

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${LIBRESSL_SOURCE_ARCHIVE}"
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        "tools" LIBRESSL_APPS
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DLIBRESSL_INSTALL_CMAKEDIR=share/${PORT}
        -DLIBRESSL_TESTS=OFF
    OPTIONS_DEBUG
        -DLIBRESSL_APPS=OFF
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()
vcpkg_cmake_config_fixup()

# libressl as openssl replacement
configure_file("${CURRENT_PORT_DIR}/vcpkg-cmake-wrapper.cmake.in" "${CURRENT_PACKAGES_DIR}/share/openssl/vcpkg-cmake-wrapper.cmake" @ONLY)

if("tools" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES ocspcheck openssl DESTINATION "${CURRENT_PACKAGES_DIR}/tools/openssl" AUTO_CLEAN)
endif()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/etc/ssl/certs"
    "${CURRENT_PACKAGES_DIR}/debug/etc/ssl/certs"
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/man"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
