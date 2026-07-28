#!/usr/bin/env bash
# Builds the Electron package: downloads Electron, copies app sources, sets
# up the launcher, copies .desktop + icons.
# Argument $1 = staging root (BUILDROOT in spec, $(DESTROOT) in deb rules).
# Pin ELECTRON_VERSION to the version your code targets.

set -xEeuo pipefail

ELECTRON_VERSION="v41.4.0"
BUILDROOT=$1
ELECTRON_URL="https://github.com/electron/electron/releases/download/$ELECTRON_VERSION/electron-$ELECTRON_VERSION-linux-x64.zip"
LIBDIR="/usr/lib/__INIT_PACKAGE_NAME__"
APPDIR=$LIBDIR/resources/app/

install -d -m755 $BUILDROOT$APPDIR
wget --no-verbose -O electron.zip $ELECTRON_URL
unzip electron.zip -d $BUILDROOT$LIBDIR/
rm electron.zip

install -d -m755 $BUILDROOT/usr/bin/

# Copy app sources into resources/app. Adjust ignore list for your project.
cp -R $(ls -I ".omnipackage" -I ".gitignore" -I ".node-version" -I "node_modules" -I "debian" -I "share") $BUILDROOT$APPDIR

# Optional: copy a share/ tree containing .desktop + icons into /usr/share/.
if [ -d share ]; then
  cp -R share/ $BUILDROOT/usr/
fi

ln -sf $LIBDIR/electron $BUILDROOT/usr/bin/__INIT_PACKAGE_NAME__

chown -R root:root $BUILDROOT$LIBDIR
chmod -R 755 $BUILDROOT$LIBDIR
