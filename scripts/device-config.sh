# Shared device selection, sourced by the other scripts.
#
# Everything here is per device in two ways: the vendor tree path
# (vendor/amazon/<device>) and the firmware dump the blobs come from. The
# tuning inside libcameracustom.so describes one specific sensor - cronos
# carries ov02b10_mipi_raw, crown and checkers carry ov9734_mipi_raw - so
# using another device's blobs yields a working camera with visibly wrong
# colour that no calibration can correct.
#
# Select with the CAMERA_DEVICE environment variable:
#
#     export CAMERA_DEVICE=crown
#
# Defaults to cronos, which is what this work was developed on.

CAMERA_DEVICE="${CAMERA_DEVICE:-cronos}"

case "$CAMERA_DEVICE" in
    cronos)
        DUMP_REPO="el-vertedero/amazon_cronos_dump"
        DUMP_BRANCH="cronos-user-6.0-NS6573-6567-amz-p,release-keys"
        ;;
    crown)
        DUMP_REPO="el-vertedero/amazon_crown_dump"
        DUMP_BRANCH="crown-user-6.0-NS6565-5565-amz-p,release-keys"
        ;;
    checkers)
        DUMP_REPO="el-vertedero/amazon_checkers_dump"
        DUMP_BRANCH="checkers-user-6.0-NS6534-2264-amz-p,release-keys"
        ;;
    *)
        echo "unknown CAMERA_DEVICE '$CAMERA_DEVICE'" >&2
        echo "expected one of: cronos, crown, checkers" >&2
        exit 2
        ;;
esac

DUMP_RAW="https://raw.githubusercontent.com/${DUMP_REPO}/${DUMP_BRANCH}"

# Where the blobs live in the build tree, and where the build puts its
# output. Both follow the device.
device_vendor_dir() { echo "$1/vendor/amazon/$CAMERA_DEVICE/proprietary"; }
device_out_dir()    { echo "$1/out/target/product/$CAMERA_DEVICE"; }
