#!/bin/bash

# Kernel versions to test (from highest to lowest)
# These match the versions in your feature.h
declare -a VERSIONS=(
    "6.11.0"
    "6.6.0"
    "6.1.0"
    "6.0.0"
    "5.17.0"
    "5.16.0"
    "5.13.0"
    "5.12.0"
    "5.11.0"
    "5.7.0"
    "4.17.0"
    "4.14.0"
    "4.13.0"
    "4.8.0"
    "4.4.0"
    "4.3.0"
    "4.2.0"
    "4.1.0"
    "4.0.0"
)

echo "Starting incremental kernel version build test"
echo "=============================================="
echo ""

for version in "${VERSIONS[@]}"; do
    echo "Testing kernel version $version"
    echo "----------------------------------------"

    # Calculate kernel version code
    # Format: (major << 16) + (minor << 8) + patch
    IFS='.' read -r major minor patch <<< "$version"
    VERSION_CODE=$((($major << 16) + ($minor << 8) + $patch))

    echo "LINUX_VERSION_CODE = $VERSION_CODE"

    # Clean previous build
    echo "Cleaning previous build..."
    make clean 2>/dev/null || true

    # Configure with overridden kernel version
    echo "Configuring..."
    if ! cmake . -DCMAKE_CXX_FLAGS="${CMAKE_CXX_FLAGS} -DPERFCPP_TEST_LINUX_VERSION_CODE=$VERSION_CODE" -DBUILD_EXAMPLES=1 -DBUILD_TESTS=0; then
        echo ""
        echo "ERROR: Configuration failed for kernel version $version"
        exit 1
    fi

    # Build
    echo "Building..."
    if ! make examples -j8; then
        echo ""
        echo "ERROR: Build failed for kernel version $version"
        exit 1
    fi

    echo "✓ Build successful for kernel version $version"
    echo ""
done

echo "=============================================="
echo "All kernel version configurations built successfully!"
echo ""