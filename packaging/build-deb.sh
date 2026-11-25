#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${1:-${ROOT_DIR}/dist}"
SKIP_TESTS="${SKIP_TESTS:-0}"

VER="$(grep -m1 'project(bluepill VERSION' "${ROOT_DIR}/CMakeLists.txt" | sed -E 's/.*VERSION ([0-9.]+).*/\1/')"
ARCH="$(dpkg --print-architecture)"
if [[ -z "${BUILD_DIR:-}" ]]; then
    BUILD_DIR="$(mktemp -d)"
    trap 'rm -rf "${BUILD_DIR}"' EXIT
fi
PKG_ROOT="${BUILD_DIR}/pkg"

mkdir -p "${OUT_DIR}"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}/cmake" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}/cmake" -j"$(nproc)"

if [[ "${SKIP_TESTS}" != "1" ]] && [[ -x "${BUILD_DIR}/cmake/imsg_tests" ]]; then
    QT_QPA_PLATFORM=offscreen ctest --test-dir "${BUILD_DIR}/cmake" --output-on-failure
fi

rm -rf "${PKG_ROOT}"
mkdir -p "${PKG_ROOT}/DEBIAN"
mkdir -p "${PKG_ROOT}/usr/bin"
mkdir -p "${PKG_ROOT}/usr/lib/systemd/user"
mkdir -p "${PKG_ROOT}/usr/share/applications"
mkdir -p "${PKG_ROOT}/usr/share/icons/hicolor/scalable/apps"
mkdir -p "${PKG_ROOT}/usr/share/pixmaps"
mkdir -p "${PKG_ROOT}/usr/share/bluepill"
mkdir -p "${PKG_ROOT}/usr/share/imsg"
mkdir -p "${PKG_ROOT}/usr/share/doc/bluepill"

install -Dm755 "${BUILD_DIR}/cmake/bluepill" "${PKG_ROOT}/usr/bin/bluepill"
ln -sf bluepill "${PKG_ROOT}/usr/bin/imsg"
install -Dm644 "${ROOT_DIR}/pkgbuild/bluepill.service" "${PKG_ROOT}/usr/lib/systemd/user/bluepill.service"
install -Dm644 "${ROOT_DIR}/pkgbuild/bluepill.desktop" "${PKG_ROOT}/usr/share/applications/bluepill.desktop"
install -Dm644 "${ROOT_DIR}/pkgbuild/bluepill.svg" "${PKG_ROOT}/usr/share/icons/hicolor/scalable/apps/bluepill.svg"
install -Dm644 "${ROOT_DIR}/assets/bluepill.png" "${PKG_ROOT}/usr/share/pixmaps/bluepill.png"
install -Dm644 "${ROOT_DIR}/resources/imessage-notify.wav" "${PKG_ROOT}/usr/share/bluepill/imessage-notify.wav"
install -Dm644 "${ROOT_DIR}/resources/imessage-notify.wav" "${PKG_ROOT}/usr/share/imsg/imessage-notify.wav"
install -Dm644 "${ROOT_DIR}/LICENSE" "${PKG_ROOT}/usr/share/doc/bluepill/copyright"

# Compute shared library dependencies via dpkg-shlibdeps when available
SHLIB_DEPS=""
if command -v dpkg-shlibdeps >/dev/null 2>&1; then
    mkdir -p "${BUILD_DIR}/debian"
    cat > "${BUILD_DIR}/debian/control" <<'EOF'
Source: bluepill
Section: net
Priority: optional
Maintainer: James Farr Gomez <james@nertwork.com>

Package: bluepill
Architecture: any
Description: Native Qt6/QML desktop & CLI client for iMessage, RCS, and SMS
EOF
    SHLIB_DEPS="$(cd "${BUILD_DIR}" && dpkg-shlibdeps -O "${PKG_ROOT}/usr/bin/bluepill" | sed -E 's/^shlibs:Depends=//')"
    rm -rf "${BUILD_DIR}/debian"
fi

EXTRA_DEPS="qml6-module-qtqml, qml6-module-qtqml-models, qml6-module-qtqml-workerscript, qml6-module-qtquick, qml6-module-qtquick-controls, qml6-module-qtquick-layouts, qml6-module-qtquick-shapes, qml6-module-qtquick-templates, qml6-module-qtquick-window, qt6-image-formats-plugins, libnotify-bin"
if [[ -n "${SHLIB_DEPS}" ]]; then
    FULL_DEPS="${SHLIB_DEPS}, ${EXTRA_DEPS}"
else
    FULL_DEPS="libqt6core6t64 | libqt6core6, libqt6gui6t64 | libqt6gui6, libqt6widgets6t64 | libqt6widgets6, libqt6qml6, libqt6quick6, libqt6quickcontrols2-6, libqt6network6t64 | libqt6network6, libqt6svg6, libqt6concurrent6t64 | libqt6concurrent6, ${EXTRA_DEPS}"
fi

cat > "${PKG_ROOT}/DEBIAN/control" <<EOF
Package: bluepill
Version: ${VER}
Section: net
Priority: optional
Architecture: ${ARCH}
Maintainer: James Farr Gomez <james@nertwork.com>
Homepage: https://github.com/nertwork/bluepill
Provides: imsg
Conflicts: imsg
Replaces: imsg
Depends: ${FULL_DEPS}
Recommends: libheif-examples, libheif-plugin-libde265, imagemagick, libsecret-tools, pulseaudio-utils | pipewire-bin, ffmpeg
Description: BluePill — Native Qt6/QML desktop & CLI client for iMessage, RCS, and SMS
 Native desktop and headless CLI client for iMessage, RCS, and SMS
 supporting the BluePill macOS Relay and BlueBubbles server APIs.
EOF

DEB_FILE="${OUT_DIR}/bluepill_${VER}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "${PKG_ROOT}" "${DEB_FILE}"
echo "Built Debian package: ${DEB_FILE}"
