#!/bin/bash
set -e

# chartscii-cli installer script
# Usage: curl -fsSL https://raw.githubusercontent.com/tool3/chartscii-cli/master/scripts/install.sh | bash

REPO="tool3/chartscii-cli"
BIN_NAME="chartscii"
INSTALL_DIR="${INSTALL_DIR:-/usr/local/bin}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
    exit 1
}

# Detect OS and architecture
detect_platform() {
    OS="$(uname -s)"
    ARCH="$(uname -m)"

    case "$OS" in
        Linux*)
            OS="linux"
            ;;
        Darwin*)
            OS="darwin"
            ;;
        MINGW*|MSYS*|CYGWIN*)
            OS="windows"
            ;;
        *)
            error "Unsupported operating system: $OS"
            ;;
    esac

    case "$ARCH" in
        x86_64|amd64)
            ARCH="x64"
            ;;
        arm64|aarch64)
            ARCH="arm64"
            ;;
        *)
            error "Unsupported architecture: $ARCH"
            ;;
    esac

    PLATFORM="${OS}-${ARCH}"
    info "Detected platform: $PLATFORM"
}

# Get the latest version from GitHub
get_latest_version() {
    info "Fetching latest version..."
    VERSION=$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" | grep '"tag_name":' | sed -E 's/.*"v([^"]+)".*/\1/')

    if [ -z "$VERSION" ]; then
        error "Failed to fetch latest version"
    fi

    info "Latest version: v$VERSION"
}

# Download and install
install() {
    ARTIFACT="${BIN_NAME}-${PLATFORM}"

    if [ "$OS" = "windows" ]; then
        DOWNLOAD_URL="https://github.com/${REPO}/releases/download/v${VERSION}/${ARTIFACT}.zip"
        ARCHIVE_EXT="zip"
    else
        DOWNLOAD_URL="https://github.com/${REPO}/releases/download/v${VERSION}/${ARTIFACT}.tar.gz"
        ARCHIVE_EXT="tar.gz"
    fi

    info "Downloading from: $DOWNLOAD_URL"

    TMP_DIR=$(mktemp -d)
    trap "rm -rf $TMP_DIR" EXIT

    cd "$TMP_DIR"

    if ! curl -fsSL "$DOWNLOAD_URL" -o "${ARTIFACT}.${ARCHIVE_EXT}"; then
        error "Failed to download release"
    fi

    info "Extracting archive..."
    if [ "$ARCHIVE_EXT" = "zip" ]; then
        unzip -q "${ARTIFACT}.${ARCHIVE_EXT}"
    else
        tar -xzf "${ARTIFACT}.${ARCHIVE_EXT}"
    fi

    # Verify checksum
    info "Verifying checksum..."
    CHECKSUM_URL="https://github.com/${REPO}/releases/download/v${VERSION}/checksums.txt"
    if curl -fsSL "$CHECKSUM_URL" -o checksums.txt 2>/dev/null; then
        EXPECTED_SHA=$(grep "${ARTIFACT}.${ARCHIVE_EXT}" checksums.txt | cut -d ' ' -f 1)
        if [ -n "$EXPECTED_SHA" ]; then
            if command -v sha256sum &> /dev/null; then
                ACTUAL_SHA=$(sha256sum "${ARTIFACT}.${ARCHIVE_EXT}" | cut -d ' ' -f 1)
            elif command -v shasum &> /dev/null; then
                ACTUAL_SHA=$(shasum -a 256 "${ARTIFACT}.${ARCHIVE_EXT}" | cut -d ' ' -f 1)
            fi

            if [ "$EXPECTED_SHA" != "$ACTUAL_SHA" ]; then
                error "Checksum verification failed!"
            fi
            success "Checksum verified"
        fi
    else
        warn "Could not verify checksum (checksums.txt not found)"
    fi

    # Install binary
    info "Installing to $INSTALL_DIR..."

    if [ ! -d "$INSTALL_DIR" ]; then
        warn "$INSTALL_DIR does not exist, attempting to create..."
        sudo mkdir -p "$INSTALL_DIR"
    fi

    if [ "$OS" = "windows" ]; then
        BIN_FILE="${ARTIFACT}.exe"
        DEST_FILE="${BIN_NAME}.exe"
    else
        BIN_FILE="$ARTIFACT"
        DEST_FILE="$BIN_NAME"
    fi

    if [ -w "$INSTALL_DIR" ]; then
        mv "$BIN_FILE" "${INSTALL_DIR}/${DEST_FILE}"
        chmod +x "${INSTALL_DIR}/${DEST_FILE}"
    else
        sudo mv "$BIN_FILE" "${INSTALL_DIR}/${DEST_FILE}"
        sudo chmod +x "${INSTALL_DIR}/${DEST_FILE}"
    fi

    success "Installed $BIN_NAME v$VERSION to ${INSTALL_DIR}/${DEST_FILE}"
}

# Verify installation
verify() {
    if command -v "$BIN_NAME" &> /dev/null; then
        success "$BIN_NAME is ready to use!"
        echo ""
        echo "Try it out:"
        echo "  echo 'Hello, World!' | $BIN_NAME -o hello.svg"
        echo ""
    else
        warn "$BIN_NAME was installed but is not in your PATH"
        echo "Add $INSTALL_DIR to your PATH or run:"
        echo "  ${INSTALL_DIR}/${BIN_NAME} --help"
    fi
}

main() {
    echo ""
    echo "  ╭──────────────────────────────╮"
    echo "  │   chartscii installer        │"
    echo "  ╰──────────────────────────────╯"
    echo ""

    detect_platform
    get_latest_version
    install
    verify
}

main
