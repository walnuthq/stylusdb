#!/bin/bash

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}=== StylusDB PKG Installer Creator ===${NC}"

# Configuration
APP_NAME="StylusDB"
APP_VERSION="0.1.0"
IDENTIFIER="com.walnuthq.stylusdb"

# Detect architecture
ARCH=$(uname -m)
if [ "$ARCH" = "arm64" ]; then
    ARCH_SUFFIX="aarch64"
elif [ "$ARCH" = "x86_64" ]; then
    ARCH_SUFFIX="x86_64"
else
    ARCH_SUFFIX="$ARCH"
fi

PKG_NAME="StylusDB-${APP_VERSION}-macOS-${ARCH_SUFFIX}.pkg"
BUILD_DIR="./build"
PKG_DIR="./pkg_build"
SCRIPTS_DIR="${PKG_DIR}/scripts"
ROOT_DIR="${PKG_DIR}/root"

# Clean previous builds
echo -e "${YELLOW}Cleaning previous builds...${NC}"
rm -rf "${PKG_DIR}"
rm -f "${PKG_NAME}"

# Build the project first if needed
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${YELLOW}Build directory doesn't exist. Running build script...${NC}"
    ./build.sh
fi

# Create package structure
echo -e "${YELLOW}Creating package structure...${NC}"
mkdir -p "${ROOT_DIR}/usr/local/bin"
mkdir -p "${ROOT_DIR}/usr/local/lib"
mkdir -p "${ROOT_DIR}/usr/local/lib/python3.13/site-packages"
mkdir -p "${SCRIPTS_DIR}"

# Copy binaries
echo -e "${YELLOW}Copying binaries...${NC}"
cp "${BUILD_DIR}/bin/stylusdb" "${ROOT_DIR}/usr/local/bin/"
cp "${BUILD_DIR}/lib/"*.dylib "${ROOT_DIR}/usr/local/lib/" 2>/dev/null || true

# Copy scripts
cp "rust-stylusdb.sh" "${ROOT_DIR}/usr/local/bin/rust-stylusdb"
chmod +x "${ROOT_DIR}/usr/local/bin/rust-stylusdb"

if [ -f "scripts/pretty_trace.py" ]; then
    cp "scripts/pretty_trace.py" "${ROOT_DIR}/usr/local/bin/pretty-print-trace"
    chmod +x "${ROOT_DIR}/usr/local/bin/pretty-print-trace"
fi

# Copy LLDB libraries if they exist
if [ -d "$HOME/.stylusdb/llvm-project-llvmorg-19.1.7/build_lldb" ]; then
    echo -e "${YELLOW}Copying LLDB libraries...${NC}"
    LLDB_BUILD="$HOME/.stylusdb/llvm-project-llvmorg-19.1.7/build_lldb"
    
    if [ -f "${LLDB_BUILD}/lib/liblldb.dylib" ]; then
        cp "${LLDB_BUILD}/lib/liblldb.dylib" "${ROOT_DIR}/usr/local/lib/liblldb.19.1.7.dylib"
    fi
    
    if [ -f "/opt/homebrew/opt/llvm@19/lib/libLLVM.dylib" ]; then
        cp "/opt/homebrew/opt/llvm@19/lib/libLLVM.dylib" "${ROOT_DIR}/usr/local/lib/"
    fi
    
    if [ -f "${LLDB_BUILD}/lib/libclang-cpp.dylib" ]; then
        cp "${LLDB_BUILD}/lib/libclang-cpp.dylib" "${ROOT_DIR}/usr/local/lib/"
    fi
    
    if [ -f "${LLDB_BUILD}/bin/lldb-argdumper" ]; then
        cp "${LLDB_BUILD}/bin/lldb-argdumper" "${ROOT_DIR}/usr/local/bin/"
    fi
    
    if [ -d "${LLDB_BUILD}/lib/python3.13/site-packages/lldb" ]; then
        cp -R "${LLDB_BUILD}/lib/python3.13/site-packages/lldb" \
            "${ROOT_DIR}/usr/local/lib/python3.13/site-packages/"
    fi
fi

# Create postinstall script
cat > "${SCRIPTS_DIR}/postinstall" << 'EOF'
#!/bin/bash

# Set proper permissions
chmod +x /usr/local/bin/stylusdb
chmod +x /usr/local/bin/rust-stylusdb
[ -f /usr/local/bin/pretty-print-trace ] && chmod +x /usr/local/bin/pretty-print-trace
[ -f /usr/local/bin/lldb-argdumper ] && chmod +x /usr/local/bin/lldb-argdumper

# Update dylib paths if needed
if [ -f /usr/local/lib/liblldb.19.1.7.dylib ]; then
    install_name_tool -id /usr/local/lib/liblldb.19.1.7.dylib /usr/local/lib/liblldb.19.1.7.dylib 2>/dev/null || true
fi

echo "StylusDB installation completed successfully!"
exit 0
EOF

chmod +x "${SCRIPTS_DIR}/postinstall"

# Create preinstall script
cat > "${SCRIPTS_DIR}/preinstall" << 'EOF'
#!/bin/bash

# Check for required dependencies
if ! command -v python3 &> /dev/null; then
    echo "Warning: Python 3 is not installed. Some features may not work."
fi

# Create directories if they don't exist
mkdir -p /usr/local/bin
mkdir -p /usr/local/lib
mkdir -p /usr/local/lib/python3.13/site-packages

exit 0
EOF

chmod +x "${SCRIPTS_DIR}/preinstall"

# Build the package
echo -e "${YELLOW}Building package...${NC}"
pkgbuild \
    --root "${ROOT_DIR}" \
    --scripts "${SCRIPTS_DIR}" \
    --identifier "${IDENTIFIER}" \
    --version "${APP_VERSION}" \
    --install-location "/" \
    "${PKG_DIR}/StylusDB.pkg"

# Create Distribution XML for productbuild
cat > "${PKG_DIR}/distribution.xml" << EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2.0">
    <title>StylusDB ${APP_VERSION}</title>
    <organization>com.walnuthq</organization>
    <domains enable_localSystem="true"/>
    <options customize="never" require-scripts="true" rootVolumeOnly="true"/>
    <license file="LICENSE" mime-type="text/plain"/>
    <readme file="README.md" mime-type="text/plain"/>
    <background file="background.png" mime-type="image/png" alignment="bottomleft" scaling="none"/>
    <welcome file="welcome.txt" mime-type="text/plain"/>
    <choices-outline>
        <line choice="default">
            <line choice="com.walnuthq.stylusdb"/>
        </line>
    </choices-outline>
    <choice id="default"/>
    <choice id="com.walnuthq.stylusdb" visible="false">
        <pkg-ref id="com.walnuthq.stylusdb"/>
    </choice>
    <pkg-ref id="com.walnuthq.stylusdb" version="${APP_VERSION}" onConclusion="none">StylusDB.pkg</pkg-ref>
</installer-gui-script>
EOF

# Create welcome text
cat > "${PKG_DIR}/welcome.txt" << 'EOF'
Welcome to StylusDB Installer

This installer will guide you through the installation of StylusDB,
a modern debugger for blockchain transactions
by Walnut https://walnut.dev/.

The following components will be installed:
• stylusdb - Main debugger executable
• rust-stylusdb - Rust debugging wrapper
• pretty-print-trace - Trace visualization tool
• Required libraries and dependencies

Installation location: /usr/local

Click Continue to proceed with the installation.
EOF

# Copy README and LICENSE if they exist
[ -f "README.md" ] && cp "README.md" "${PKG_DIR}/"
[ -f "LICENSE" ] && cp "LICENSE" "${PKG_DIR}/"

# Build final product archive
echo -e "${YELLOW}Creating final installer package...${NC}"
productbuild \
    --distribution "${PKG_DIR}/distribution.xml" \
    --package-path "${PKG_DIR}" \
    --resources "${PKG_DIR}" \
    "${PKG_NAME}"

# Clean up
rm -rf "${PKG_DIR}"

echo -e "${GREEN}✅ Package created successfully: ${PKG_NAME}${NC}"
echo -e "${GREEN}Size: $(du -h ${PKG_NAME} | cut -f1)${NC}"
echo ""
echo -e "${YELLOW}To install:${NC}"
echo "1. Double-click ${PKG_NAME}"
echo "2. Follow the installation wizard"
echo "3. The installer will place files in /usr/local"
echo ""
echo -e "${YELLOW}To distribute:${NC}"
echo "• Upload ${PKG_NAME} to GitHub Releases"
echo "• Users can download and install directly"