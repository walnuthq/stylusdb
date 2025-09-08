# walnut-dbg
Modern debugger for blockchain transactions.

## Quick Start

### Option 1: Automated Build Script (Recommended)

Clone the repository and run the automated build script:

```bash
git clone https://github.com/walnuthq/walnut-dbg.git
cd walnut-dbg
./build.sh
```

The script will:
- Install all required dependencies automatically
- Download and build LLDB if needed (macOS only, cached for future builds)
- Build walnut-dbg
- Optionally install it system-wide

### Option 2: Download Pre-built Binaries (Easiest)

TODO: Add binaries into repo/releases.

### Option 3: Manual Build

<details>
<summary>Click to expand manual build instructions</summary>

#### Prerequisites

##### macOS
```bash
brew install llvm@19 lit swig ninja cmake
```

##### Linux (Ubuntu/Debian)
```bash
wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- 19
sudo apt-get install -y cmake ninja-build llvm-19-dev liblldb-19-dev lldb-19 swig
```

#### Build Steps

##### macOS Only: Build LLDB from source
```bash
wget https://github.com/llvm/llvm-project/archive/refs/tags/llvmorg-19.1.7.zip
unzip llvmorg-19.1.7.zip
cd llvm-project-llvmorg-19.1.7/ && mkdir build_lldb && cd build_lldb
cmake ../llvm -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS="clang;lldb" \
  -DLLVM_ENABLE_ASSERTIONS=ON -DLLDB_INCLUDE_TESTS=OFF -DLLDB_ENABLE_PYTHON=1 -GNinja
ninja
```

##### Build walnut-dbg

###### macOS
```bash
cd /path/to/walnut-dbg
mkdir build && cd build
cmake -GNinja .. \
  -DLLVM_DIR=/opt/homebrew/opt/llvm@19/lib/cmake/llvm \
  -DLLVM_BUILD_ROOT=/path/to/llvm-project-llvmorg-19.1.7/build_lldb \
  -DLLVM_SRC=/path/to/llvm-project-llvmorg-19.1.7/ \
  -DLLVM_TABLEGEN_EXE=/opt/homebrew/opt/llvm@19/bin/llvm-tblgen \
  -DCMAKE_CXX_FLAGS="-Wno-deprecated-declarations" \
  -DLLVM_LIB_PATH=/opt/homebrew/opt/llvm@19/lib/libLLVM.dylib
sudo ninja && sudo ninja install
```

###### Linux
```bash
cd /path/to/walnut-dbg
mkdir build && cd build
cmake -GNinja .. \
  -DLLVM_DIR=/usr/lib/llvm-19/lib/cmake/llvm \
  -DLLVM_BUILD_ROOT=/usr/lib/llvm-19 \
  -DLLVM_SRC=/usr/include/llvm-19 \
  -DCMAKE_CXX_FLAGS="-Wno-deprecated-declarations"
sudo ninja && sudo ninja install
```

</details>

## Usage

### Basic Usage
```bash
/usr/local/bin/walnut-dbg
(walnut-dbg) target create ./my-program
(walnut-dbg) run
(walnut-dbg) quit
```

### With cargo-stylus
```bash
/usr/local/bin/rust-walnut-dbg ./my-rust-program
```

### Pretty Print Traces
```bash
python3 -m venv myvenv
source ./myvenv/bin/activate
pip3 install colorama
/usr/local/bin/pretty-print-trace /tmp/lldb_function_trace.json
```

## Troubleshooting

### macOS: "liblldb.dylib not found"
The automated build script handles this, but if building manually, ensure LLDB libraries are copied to `/usr/local/lib/`.

### Linux: Missing LLDB headers
Install the development packages:
```bash
sudo apt-get install liblldb-19-dev lldb-19
```

## License

See [LICENSE](LICENSE) file for details.
