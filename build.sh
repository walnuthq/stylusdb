#!/bin/bash

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}=== Walnut-DBG Build Script ===${NC}"

OS_TYPE=""
if [[ "$OSTYPE" == "darwin"* ]]; then
    OS_TYPE="macos"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    OS_TYPE="linux"
else
    echo -e "${RED}Unsupported OS: $OSTYPE${NC}"
    exit 1
fi

echo -e "${GREEN}Detected OS: $OS_TYPE${NC}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
LLDB_BUILD_DIR=""
LLVM_SRC_DIR=""
USE_PREBUILT_LLDB=false

check_command() {
    if ! command -v "$1" &> /dev/null; then
        echo -e "${RED}$1 is not installed${NC}"
        return 1
    fi
    return 0
}

check_prerequisites() {
    echo -e "${YELLOW}Checking essential build tools...${NC}"
    
    local missing_tools=""
    
    # Check essential tools that cannot be auto-installed
    if ! check_command git; then
        missing_tools="$missing_tools git"
    fi
    
    if ! check_command make; then
        missing_tools="$missing_tools make"
    fi
    
    if ! check_command python3; then
        missing_tools="$missing_tools python3"
    fi
    
    if [[ "$OS_TYPE" == "macos" ]]; then
        # Check for Xcode Command Line Tools
        if ! xcode-select -p &>/dev/null; then
            echo -e "${RED}Xcode Command Line Tools not installed${NC}"
            echo -e "${YELLOW}Installing Xcode Command Line Tools...${NC}"
            xcode-select --install
            echo -e "${YELLOW}Please complete the installation and re-run this script${NC}"
            exit 1
        fi
        
        # Check for MIG tool (needed for debugserver)
        if ! check_command mig; then
            missing_tools="$missing_tools mig"
        fi
    fi
    
    if [ -n "$missing_tools" ]; then
        echo -e "${RED}Missing required tools:$missing_tools${NC}"
        echo -e "${YELLOW}Please install these tools and re-run the script${NC}"
        exit 1
    fi
    
    echo -e "${GREEN}All essential tools found${NC}"
}

install_dependencies_macos() {
    echo -e "${YELLOW}Checking dependencies for macOS...${NC}"
    
    if ! check_command brew; then
        echo -e "${RED}Homebrew is required. Install from https://brew.sh${NC}"
        exit 1
    fi
    
    if ! brew list llvm@19 &>/dev/null; then
        echo -e "${YELLOW}Installing LLVM 19...${NC}"
        brew install llvm@19
    fi
    
    if ! check_command ninja; then
        echo -e "${YELLOW}Installing ninja...${NC}"
        brew install ninja
    fi
    
    if ! check_command cmake; then
        echo -e "${YELLOW}Installing cmake...${NC}"
        brew install cmake
    fi
    
    if ! check_command swig; then
        echo -e "${YELLOW}Installing swig...${NC}"
        brew install swig
    fi
    
    if ! check_command lit; then
        echo -e "${YELLOW}Installing lit...${NC}"
        brew install lit
    fi
    
    # Check cmake version
    CMAKE_VERSION=$(cmake --version | head -n1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || echo "0.0.0")
    CMAKE_MAJOR=$(echo $CMAKE_VERSION | cut -d. -f1)
    CMAKE_MINOR=$(echo $CMAKE_VERSION | cut -d. -f2)
    
    if [ "$CMAKE_MAJOR" -lt 3 ] || ([ "$CMAKE_MAJOR" -eq 3 ] && [ "$CMAKE_MINOR" -lt 20 ]); then
        echo -e "${YELLOW}CMake version $CMAKE_VERSION is too old. Updating...${NC}"
        brew upgrade cmake
    fi
}

install_dependencies_linux() {
    echo -e "${YELLOW}Checking dependencies for Linux...${NC}"
    
    DISTRO=$(lsb_release -si 2>/dev/null || echo "Unknown")
    
    if [[ "$DISTRO" == "Ubuntu" ]] || [[ "$DISTRO" == "Debian" ]]; then
        MISSING_DEPS=""
        
        check_command cmake || MISSING_DEPS="$MISSING_DEPS cmake"
        check_command ninja-build || check_command ninja || MISSING_DEPS="$MISSING_DEPS ninja-build"
        check_command swig || MISSING_DEPS="$MISSING_DEPS swig"
        
        dpkg -l | grep -q llvm-19-dev || MISSING_DEPS="$MISSING_DEPS llvm-19-dev"
        dpkg -l | grep -q liblldb-19-dev || MISSING_DEPS="$MISSING_DEPS liblldb-19-dev"
        dpkg -l | grep -q lldb-19 || MISSING_DEPS="$MISSING_DEPS lldb-19"
        
        if [ -n "$MISSING_DEPS" ]; then
            echo -e "${YELLOW}Installing missing dependencies: $MISSING_DEPS${NC}"
            sudo apt-get update
            sudo apt-get install -y $MISSING_DEPS
        fi
    else
        echo -e "${YELLOW}Please install: cmake, ninja, llvm-19, lldb-19-dev, swig${NC}"
        echo -e "${YELLOW}Continuing anyway...${NC}"
    fi
}

check_or_download_lldb_source() {
    if [[ "$OS_TYPE" == "linux" ]]; then
        echo -e "${GREEN}Linux: Using system LLDB packages${NC}"
        return 0
    fi
    
    LLDB_VERSION="19.1.7"
    LLDB_ARCHIVE="llvmorg-${LLDB_VERSION}.zip"
    LLDB_DIR="llvm-project-llvmorg-${LLDB_VERSION}"
    
    echo -e "${YELLOW}Checking for pre-built LLDB...${NC}"
    
    # Check for existing LLDB build in the correct location
    if [ -f "$HOME/.walnut-dbg/${LLDB_DIR}/build_lldb/lib/liblldb.dylib" ]; then
        echo -e "${GREEN}Found existing LLDB build at $HOME/.walnut-dbg/${LLDB_DIR}${NC}"
        LLDB_BUILD_DIR="$HOME/.walnut-dbg/${LLDB_DIR}/build_lldb"
        LLVM_SRC_DIR="$HOME/.walnut-dbg/${LLDB_DIR}"
        USE_PREBUILT_LLDB=true
        return 0
    fi
    
    # Also check for the renamed directory (in case it was already renamed)
    if [ -f "$HOME/.walnut-dbg/lldb-${LLDB_VERSION}-macos/build_lldb/lib/liblldb.dylib" ]; then
        echo -e "${GREEN}Found pre-built LLDB at $HOME/.walnut-dbg/lldb-${LLDB_VERSION}-macos${NC}"
        LLDB_BUILD_DIR="$HOME/.walnut-dbg/lldb-${LLDB_VERSION}-macos/build_lldb"
        LLVM_SRC_DIR="$HOME/.walnut-dbg/lldb-${LLDB_VERSION}-macos"
        USE_PREBUILT_LLDB=true
        return 0
    fi
    
    echo -e "${YELLOW}LLDB source/build not found. We need to build it from source.${NC}"
    echo -e "${YELLOW}This will take 30-60 minutes but only needs to be done once.${NC}"
    echo -n "Continue? [Y/n]: "
    read -r response
    
    if [[ "$response" =~ ^[Nn]$ ]]; then
        echo -e "${RED}Build cancelled${NC}"
        exit 1
    fi
    
    mkdir -p "$HOME/.walnut-dbg"
    cd "$HOME/.walnut-dbg"
    
    if [ ! -d "$LLDB_DIR" ]; then
        echo -e "${YELLOW}Downloading LLVM/LLDB ${LLDB_VERSION}...${NC}"
        if ! command -v wget &> /dev/null; then
            echo -e "${YELLOW}wget not found, using curl instead...${NC}"
            curl -L -o "$LLDB_ARCHIVE" "https://github.com/llvm/llvm-project/archive/refs/tags/${LLDB_ARCHIVE}"
        else
            wget "https://github.com/llvm/llvm-project/archive/refs/tags/${LLDB_ARCHIVE}"
        fi
        unzip -q "$LLDB_ARCHIVE"
        rm "$LLDB_ARCHIVE"
    fi
    
    cd "$LLDB_DIR"
    
    if [ ! -d "build_lldb" ]; then
        echo -e "${YELLOW}Building LLDB (this will take 30-60 minutes)...${NC}"
        mkdir build_lldb
        cd build_lldb
        
        # Determine SDK path for macOS
        CMAKE_ARGS=""
        if [[ "$OS_TYPE" == "macos" ]]; then
            # Find the SDK path
            SDK_PATH=$(xcrun --show-sdk-path 2>/dev/null || echo "")
            if [ -n "$SDK_PATH" ]; then
                CMAKE_ARGS="-DCMAKE_OSX_SYSROOT=$SDK_PATH"
                echo -e "${GREEN}Using macOS SDK: $SDK_PATH${NC}"
            fi
        fi
        
        cmake ../llvm \
            -DCMAKE_BUILD_TYPE=Release \
            -DLLVM_ENABLE_PROJECTS="clang;lldb" \
            -DLLVM_ENABLE_ASSERTIONS=ON \
            -DLLDB_INCLUDE_TESTS=OFF \
            -DLLDB_ENABLE_PYTHON=1 \
            -GNinja \
            $CMAKE_ARGS
        
        # Build LLDB
        echo -e "${YELLOW}Starting LLDB build...${NC}"
        ninja -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
        
        echo -e "${GREEN}LLDB build complete!${NC}"
    fi
    
    LLDB_BUILD_DIR="$HOME/.walnut-dbg/${LLDB_DIR}/build_lldb"
    LLVM_SRC_DIR="$HOME/.walnut-dbg/${LLDB_DIR}"
}

build_walnut_dbg() {
    echo -e "${GREEN}Building walnut-dbg...${NC}"
    
    cd "$SCRIPT_DIR"
    
    if [ -d "$BUILD_DIR" ]; then
        echo -e "${YELLOW}Removing old build directory...${NC}"
        rm -rf "$BUILD_DIR"
    fi
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    CMAKE_ARGS="-GNinja"
    
    if [[ "$OS_TYPE" == "macos" ]]; then
        LLVM_DIR="/opt/homebrew/opt/llvm@19/lib/cmake/llvm"
        LLVM_LIB_PATH="/opt/homebrew/opt/llvm@19/lib/libLLVM.dylib"
        LLVM_TABLEGEN="/opt/homebrew/opt/llvm@19/bin/llvm-tblgen"
        
        if [ ! -f "$LLVM_DIR/LLVMConfig.cmake" ]; then
            LLVM_DIR="/usr/local/opt/llvm@19/lib/cmake/llvm"
            LLVM_LIB_PATH="/usr/local/opt/llvm@19/lib/libLLVM.dylib"
            LLVM_TABLEGEN="/usr/local/opt/llvm@19/bin/llvm-tblgen"
        fi
        
        CMAKE_ARGS="$CMAKE_ARGS \
            -DLLVM_DIR=$LLVM_DIR \
            -DLLVM_BUILD_ROOT=$LLDB_BUILD_DIR \
            -DLLVM_SRC=$LLVM_SRC_DIR \
            -DLLVM_TABLEGEN_EXE=$LLVM_TABLEGEN \
            -DCMAKE_CXX_FLAGS=-Wno-deprecated-declarations \
            -DLLVM_LIB_PATH=$LLVM_LIB_PATH"
    else
        CMAKE_ARGS="$CMAKE_ARGS \
            -DLLVM_DIR=/usr/lib/llvm-19/lib/cmake/llvm \
            -DLLVM_BUILD_ROOT=/usr/lib/llvm-19 \
            -DLLVM_SRC=/usr/include/llvm-19 \
            -DCMAKE_CXX_FLAGS=-Wno-deprecated-declarations"
    fi
    
    echo -e "${YELLOW}Running cmake...${NC}"
    cmake $CMAKE_ARGS ..
    
    echo -e "${YELLOW}Building...${NC}"
    ninja
    
    echo -e "${GREEN}Build complete!${NC}"
    echo -e "${YELLOW}To install system-wide, run: cd $BUILD_DIR && sudo ninja install${NC}"
}

install_walnut_dbg() {
    echo -n "Install walnut-dbg system-wide? (requires sudo) [Y/n]: "
    read -r response
    
    if [[ ! "$response" =~ ^[Nn]$ ]]; then
        cd "$BUILD_DIR"
        echo -e "${YELLOW}Installing walnut-dbg...${NC}"
        sudo ninja install
        echo -e "${GREEN}Installation complete!${NC}"
        echo -e "${GREEN}You can now run: /usr/local/bin/walnut-dbg${NC}"
    else
        echo -e "${YELLOW}Skipping installation. Binaries are in: $BUILD_DIR/bin${NC}"
    fi
}

main() {
    # Check prerequisites first
    check_prerequisites
    
    if [[ "$OS_TYPE" == "macos" ]]; then
        install_dependencies_macos
        check_or_download_lldb_source
    else
        install_dependencies_linux
    fi
    
    build_walnut_dbg
    install_walnut_dbg
    
    echo -e "${GREEN}=== Build process complete! ===${NC}"
}

main "$@"