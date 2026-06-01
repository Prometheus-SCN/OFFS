#!/bin/bash
set -euo pipefail

PLATFORM="${1:-}"
TAG="${2:-}"

if [ -z "$PLATFORM" ] || [ -z "$TAG" ]; then
  echo "Usage: $0 <linux-x64|macos-x64|windows-x64> <tag>"
  echo ""
  echo "Run on each platform to build and package the release."
  echo "  linux-x64   - Build .tar.gz, .deb, .rpm on Linux"
  echo "  macos-x64   - Build .tar.gz, .pkg on macOS"
  echo "  windows-x64 - Build .zip, .msi on Windows"
  exit 1
fi

echo "=== OFFS Release Builder ==="
echo "Platform: $PLATFORM"
echo "Tag: $TAG"

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build-release"
BUNDLE="offs-${PLATFORM}"
ARTIFACTS_DIR="$PROJECT_DIR/artifacts"

# Clean and build
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR" "$ARTIFACTS_DIR"

cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release

# Detect core count
if command -v nproc &>/dev/null; then
  CORES=$(nproc)
elif command -v sysctl &>/dev/null; then
  CORES=$(sysctl -n hw.ncpu)
else
  CORES=4
fi

cmake --build . -j"$CORES"

# Create bundle directory
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"

case "$PLATFORM" in
  linux-x64)
    # Copy binaries from OFFS build
    if [ -f "$BUILD_DIR/offs-updater" ]; then
      cp "$BUILD_DIR/offs-updater" "$BUNDLE/offs-updater"
    fi
    if [ -f "$BUILD_DIR/offsd" ]; then
      cp "$BUILD_DIR/offsd" "$BUNDLE/offs-daemon"
    fi
    if [ -f "$BUILD_DIR/offs_cli" ]; then
      cp "$BUILD_DIR/offs_cli" "$BUNDLE/offs-cli"
    fi

    # Version file
    echo "$TAG" > "$BUNDLE/VERSION"

    # Icon
    mkdir -p "$BUNDLE/share/icons/hicolor/256x256/apps"
    cp "$PROJECT_DIR/packaging/linux/offs.png" "$BUNDLE/share/icons/hicolor/256x256/apps/"

    # Checksums
    cd "$BUNDLE" && sha256sum $(find . -type f | sed 's|^\./||') > checksums.sha256 && cd ..

    # Create tarball
    tar -czf "$ARTIFACTS_DIR/${BUNDLE}.tar.gz" "$BUNDLE"

    # Build .deb
    echo "Building .deb package..."
    DEB_DIR="$BUILD_DIR/deb/offs_${TAG#v}_amd64"
    mkdir -p "$DEB_DIR/DEBIAN" "$DEB_DIR/usr/bin" "$DEB_DIR/usr/lib/systemd/system" "$DEB_DIR/usr/share/icons/hicolor/256x256/apps"
    cp "$PROJECT_DIR/packaging/linux/debian/control" "$DEB_DIR/DEBIAN/"
    sed -i "s/Version: .*/Version: ${TAG#v}/" "$DEB_DIR/DEBIAN/control"
    cp "$PROJECT_DIR/packaging/linux/debian/postinst" "$DEB_DIR/DEBIAN/"
    cp "$PROJECT_DIR/packaging/linux/debian/prerm" "$DEB_DIR/DEBIAN/"
    cp "$PROJECT_DIR/packaging/linux/debian/postrm" "$DEB_DIR/DEBIAN/"
    cp "$BUNDLE/offs-daemon" "$DEB_DIR/usr/bin/"
    cp "$BUNDLE/offs-cli" "$DEB_DIR/usr/bin/"
    cp "$BUNDLE/offs-updater" "$DEB_DIR/usr/bin/"
    cp "$PROJECT_DIR/packaging/linux/debian/offs-daemon.service" "$DEB_DIR/usr/lib/systemd/system/"
    cp "$PROJECT_DIR/packaging/linux/offs.png" "$DEB_DIR/usr/share/icons/hicolor/256x256/apps/"
    chmod 755 "$DEB_DIR/DEBIAN/postinst" "$DEB_DIR/DEBIAN/prerm" "$DEB_DIR/DEBIAN/postrm"
    dpkg-deb --build "$DEB_DIR" "$ARTIFACTS_DIR/offs_${TAG#v}_amd64.deb"
    echo "  -> $ARTIFACTS_DIR/offs_${TAG#v}_amd64.deb"

    # Build .rpm
    echo "Building .rpm package..."
    RPMBUILD_DIR="$BUILD_DIR/rpmbuild"
    mkdir -p "$RPMBUILD_DIR/SOURCES"
    cp "$ARTIFACTS_DIR/${BUNDLE}.tar.gz" "$RPMBUILD_DIR/SOURCES/offs-${TAG#v}.tar.gz"
    rpmbuild -ba "$PROJECT_DIR/packaging/linux/rpm/offs.spec" \
      --define "_topdir $RPMBUILD_DIR" \
      --define "version ${TAG#v}" \
      -D"_sourcedir $RPMBUILD_DIR/SOURCES" 2>/dev/null || \
      echo "  WARNING: rpmbuild not available, skipping .rpm"

    # Relay server packages
    RELAY_BUNDLE="offs-relay-${PLATFORM}"
    rm -rf "$RELAY_BUNDLE"
    mkdir -p "$RELAY_BUNDLE"
    if [ -f "$BUILD_DIR/deps/liboffs/src/Network/Relay/offs_relay" ]; then
      cp "$BUILD_DIR/deps/liboffs/src/Network/Relay/offs_relay" "$RELAY_BUNDLE/offs_relay"
    fi
    echo "$TAG" > "$RELAY_BUNDLE/VERSION"
    cd "$RELAY_BUNDLE" && sha256sum * > checksums.sha256 && cd ..
    tar -czf "$ARTIFACTS_DIR/${RELAY_BUNDLE}.tar.gz" "$RELAY_BUNDLE"

    echo "Building relay .deb..."
    RELAY_DEB_DIR="$BUILD_DIR/deb/offs-relay_${TAG#v}_amd64"
    mkdir -p "$RELAY_DEB_DIR/DEBIAN" "$RELAY_DEB_DIR/usr/bin"
    cp "$PROJECT_DIR/packaging/relay/linux/debian/control" "$RELAY_DEB_DIR/DEBIAN/"
    sed -i "s/Version: .*/Version: ${TAG#v}/" "$RELAY_DEB_DIR/DEBIAN/control"
    cp "$RELAY_BUNDLE/offs_relay" "$RELAY_DEB_DIR/usr/bin/"
    dpkg-deb --build "$RELAY_DEB_DIR" "$ARTIFACTS_DIR/offs-relay_${TAG#v}_amd64.deb"
    echo "  -> $ARTIFACTS_DIR/offs-relay_${TAG#v}_amd64.deb"

    echo "Building relay .rpm..."
    RELAY_RPMBUILD_DIR="$BUILD_DIR/relay-rpmbuild"
    mkdir -p "$RELAY_RPMBUILD_DIR/SOURCES"
    cp "$ARTIFACTS_DIR/${RELAY_BUNDLE}.tar.gz" "$RELAY_RPMBUILD_DIR/SOURCES/offs-relay-${TAG#v}.tar.gz"
    rpmbuild -ba "$PROJECT_DIR/packaging/relay/linux/rpm/offs-relay.spec" \
      --define "_topdir $RELAY_RPMBUILD_DIR" \
      --define "version ${TAG#v}" \
      -D"_sourcedir $RELAY_RPMBUILD_DIR/SOURCES" 2>/dev/null || \
      echo "  WARNING: rpmbuild not available, skipping relay .rpm"
    ;;

  macos-x64)
    if [ -f "$BUILD_DIR/offs-updater" ]; then
      cp "$BUILD_DIR/offs-updater" "$BUNDLE/offs-updater"
    fi
    if [ -f "$BUILD_DIR/offsd" ]; then
      cp "$BUILD_DIR/offsd" "$BUNDLE/offs-daemon"
    fi
    if [ -f "$BUILD_DIR/offs_cli" ]; then
      cp "$BUILD_DIR/offs_cli" "$BUNDLE/offs-cli"
    fi
    echo "$TAG" > "$BUNDLE/VERSION"
    mkdir -p "$BUNDLE/share/icons"
    cp "$PROJECT_DIR/packaging/macos/offs.icns" "$BUNDLE/share/icons/"
    cd "$BUNDLE" && shasum -a 256 $(find . -type f | sed 's|^\./||') > checksums.sha256 && cd ..
    tar -czf "$ARTIFACTS_DIR/${BUNDLE}.tar.gz" "$BUNDLE"

    # Build .pkg
    if command -v pkgbuild &>/dev/null; then
      echo "Building .pkg..."
      mkdir -p "$BUILD_DIR/pkg/root/usr/local/bin"
      cp "$BUNDLE/offs-daemon" "$BUILD_DIR/pkg/root/usr/local/bin/"
      cp "$BUNDLE/offs-cli" "$BUILD_DIR/pkg/root/usr/local/bin/"
      cp "$BUNDLE/offs-updater" "$BUILD_DIR/pkg/root/usr/local/bin/"
      pkgbuild --root "$BUILD_DIR/pkg/root" \
        --scripts "$PROJECT_DIR/packaging/macos" \
        --identifier com.offs.daemon \
        --version "${TAG#v}" \
        "$ARTIFACTS_DIR/offs-${TAG#v}.pkg"
      echo "  -> $ARTIFACTS_DIR/offs-${TAG#v}.pkg"
    else
      echo "WARNING: pkgbuild not available, skipping .pkg"
    fi

    # Relay server package
    RELAY_BUNDLE="offs-relay-${PLATFORM}"
    rm -rf "$RELAY_BUNDLE"
    mkdir -p "$RELAY_BUNDLE"
    if [ -f "$BUILD_DIR/deps/liboffs/src/Network/Relay/offs_relay" ]; then
      cp "$BUILD_DIR/deps/liboffs/src/Network/Relay/offs_relay" "$RELAY_BUNDLE/offs_relay"
    fi
    echo "$TAG" > "$RELAY_BUNDLE/VERSION"
    cd "$RELAY_BUNDLE" && shasum -a 256 $(find . -type f | sed 's|^\./||') > checksums.sha256 && cd ..
    tar -czf "$ARTIFACTS_DIR/${RELAY_BUNDLE}.tar.gz" "$RELAY_BUNDLE"

    if command -v pkgbuild &>/dev/null; then
      echo "Building relay .pkg..."
      mkdir -p "$BUILD_DIR/relay-pkg/root/usr/local/bin"
      cp "$RELAY_BUNDLE/offs_relay" "$BUILD_DIR/relay-pkg/root/usr/local/bin/"
      pkgbuild --root "$BUILD_DIR/relay-pkg/root" \
        --scripts "$PROJECT_DIR/packaging/relay/macos" \
        --identifier com.offs.relay \
        --version "${TAG#v}" \
        "$ARTIFACTS_DIR/offs-relay-${TAG#v}.pkg"
      echo "  -> $ARTIFACTS_DIR/offs-relay-${TAG#v}.pkg"
    fi
    ;;

  windows-x64)
    if [ -f "$BUILD_DIR/offs-updater.exe" ]; then
      cp "$BUILD_DIR/offs-updater.exe" "$BUNDLE/offs-updater.exe"
    fi
    if [ -f "$BUILD_DIR/offsd.exe" ]; then
      cp "$BUILD_DIR/offsd.exe" "$BUNDLE/offs-daemon.exe"
    fi
    if [ -f "$BUILD_DIR/offs_cli.exe" ]; then
      cp "$BUILD_DIR/offs_cli.exe" "$BUNDLE/offs-cli.exe"
    fi
    echo "$TAG" > "$BUNDLE/VERSION"
    cd "$BUNDLE" && sha256sum * > checksums.sha256 && cd ..
    zip -r "$ARTIFACTS_DIR/${BUNDLE}.zip" "$BUNDLE"

    # Build .msi
    if command -v candle &>/dev/null && command -v light &>/dev/null; then
      echo "Building .msi..."
      cd "$PROJECT_DIR/packaging/windows"
      candle offs.wxs -dVersion="${TAG#v}" -o "$BUILD_DIR/"
      light "$BUILD_DIR/offs.wixobj" -o "$ARTIFACTS_DIR/offs-${TAG#v}.msi"
      echo "  -> $ARTIFACTS_DIR/offs-${TAG#v}.msi"
    else
      echo "WARNING: WiX Toolset not available, skipping .msi"
    fi

    # Relay server package
    RELAY_BUNDLE="offs-relay-${PLATFORM}"
    rm -rf "$RELAY_BUNDLE"
    mkdir -p "$RELAY_BUNDLE"
    if [ -f "$BUILD_DIR/deps/liboffs/src/Network/Relay/offs_relay.exe" ]; then
      cp "$BUILD_DIR/deps/liboffs/src/Network/Relay/offs_relay.exe" "$RELAY_BUNDLE/offs_relay.exe"
    fi
    echo "$TAG" > "$RELAY_BUNDLE/VERSION"
    cd "$RELAY_BUNDLE" && sha256sum * > checksums.sha256 && cd ..
    zip -r "$ARTIFACTS_DIR/${RELAY_BUNDLE}.zip" "$RELAY_BUNDLE"

    if command -v candle &>/dev/null && command -v light &>/dev/null; then
      echo "Building relay .msi..."
      cd "$PROJECT_DIR/packaging/relay/windows"
      candle offs-relay.wxs -dVersion="${TAG#v}" -o "$BUILD_DIR/"
      light "$BUILD_DIR/offs-relay.wixobj" -o "$ARTIFACTS_DIR/offs-relay-${TAG#v}.msi"
      echo "  -> $ARTIFACTS_DIR/offs-relay-${TAG#v}.msi"
    fi
    ;;
esac

echo ""
echo "=== Build complete ==="
echo "Artifacts in: $ARTIFACTS_DIR"
ls -la "$ARTIFACTS_DIR/"
