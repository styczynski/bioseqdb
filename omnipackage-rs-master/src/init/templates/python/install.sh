#!/usr/bin/env bash
# Build/install wrapper for the Python project.
# Argument $1 = staging root (BUILDROOT in spec, $(DESTROOT) in deb rules).
# Edit this script to match how YOUR project should be deployed.

set -xEeuo pipefail

BUILDROOT=$1
LIBDIR="/usr/lib/__INIT_PACKAGE_NAME__"

install -d -m755 $BUILDROOT$LIBDIR
install -d -m755 $BUILDROOT/usr/bin/

# Copy your source files into the lib dir. Adjust ignore list as needed.
cp -R $(ls -I ".omnipackage" -I ".gitignore" -I ".python-version" -I "node_modules" -I "debian") $BUILDROOT$LIBDIR

cd $BUILDROOT$LIBDIR/

# Vendor python deps into ./vendorlibs (loaded via PYTHONPATH at runtime).
if [ -f requirements.txt ]; then
  pip3 install --target ./vendorlibs --upgrade -r requirements.txt
fi

# Generate the launcher binary in /usr/bin/.
cat > $BUILDROOT/usr/bin/__INIT_PACKAGE_NAME__ <<EOF
#!/usr/bin/env bash
export PYTHONPATH=$LIBDIR/vendorlibs
cd $LIBDIR && python3 main.py "\$@"
EOF
chmod +x $BUILDROOT/usr/bin/__INIT_PACKAGE_NAME__
