vcpkg_download_distfile(sdk_msi
    URLS "https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-${VERSION}.msi"
    FILENAME "winfsp-${VERSION}.msi"
    SHA512 48b37336721035acb67c57f2938337818e5a83e01aa4b91163672627044abfaa0cd6becac96c0c387c4e73a8669a38fbab87e400b8a133493032d4536833bb02
)

# Extract the signed release without running its installer or registering a driver.
set(extraction_directory "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}")
file(MAKE_DIRECTORY "${extraction_directory}")
cmake_path(NATIVE_PATH sdk_msi native_msi)
vcpkg_execute_required_process(
    COMMAND "${LESSMSI}" x "${native_msi}"
    WORKING_DIRECTORY "${extraction_directory}"
    LOGNAME "extract-${TARGET_TRIPLET}"
)

set(sdk_root "${extraction_directory}/winfsp-${VERSION}/SourceDir/DYNAMIC")
file(INSTALL "${sdk_root}/inc/winfsp" DESTINATION "${CURRENT_PACKAGES_DIR}/include")

# The import library is identical for Debug and Release. Runtime DLLs stay with the installed driver.
file(INSTALL "${sdk_root}/lib/winfsp-x64.lib" DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
file(INSTALL "${sdk_root}/lib/winfsp-x64.lib" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
vcpkg_install_copyright(FILE_LIST "${sdk_root}/License.txt")
