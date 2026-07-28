#!/usr/bin/env bash
# Build/install wrapper for the Ruby gem.
# Argument $1 = staging root (BUILDROOT in spec, $(DESTROOT) in deb rules).
# Edit this script to match how YOUR gem should be deployed.

set -xEeuo pipefail

BUILDROOT=$1
LIBDIR="/usr/lib/__INIT_PACKAGE_NAME__"

install -d -m755 $BUILDROOT$LIBDIR
install -d -m755 $BUILDROOT/usr/bin/

# Copy your source into the lib dir. Adjust ignore list as needed.
cp -R $(ls -I ".omnipackage" -I ".gitignore" -I ".ruby-version" -I "node_modules" -I "debian") $BUILDROOT$LIBDIR

cd $BUILDROOT$LIBDIR/

# Configure bundler to vendor gems alongside the app.
mkdir -p .bundle
cat > .bundle/config <<'EOF'
---
BUNDLE_PATH: "vendor/bundle"
BUNDLE_WITHOUT: "development"
EOF

bundle install

# Generate the launcher binary in /usr/bin/. Edit the exe path below.
cat > $BUILDROOT/usr/bin/__INIT_PACKAGE_NAME__ <<EOF
#!/usr/bin/env bash
cd $LIBDIR && bundle exec ruby exe/__INIT_PACKAGE_NAME__ "\$@"
EOF
chmod +x $BUILDROOT/usr/bin/__INIT_PACKAGE_NAME__
