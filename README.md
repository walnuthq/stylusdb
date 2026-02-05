# stylusdb
Modern debugger for blockchain transactions.

## Quick Start

### Option 1: Automated Build Script (Recommended)

Clone the repository and run the automated build script:

```bash
git clone https://github.com/walnuthq/stylusdb.git
cd stylusdb
./build.sh
```

The script will:
- Install all required dependencies automatically
- Download and build LLDB if needed (macOS only, cached for future builds)
- Build stylusdb
- Optionally install it system-wide

### Option 2: Download Pre-built Binaries (Easiest)

Download the latest installer from [GitHub Releases](https://github.com/walnuthq/stylusdb/releases):

1. Download the appropriate package for your system:
   - **Apple Silicon (M1/M2/M3)**: `StylusDB-0.1.0-macOS-aarch64.pkg`
   - **Intel**: `StylusDB-0.1.0-macOS-x86_64.pkg`
2. Double-click the package to install
3. Follow the installation wizard

The package includes:
- `stylusdb` - Main debugger
- `rust-stylusdb` - Rust debugging wrapper
- `pretty-print-trace` - Trace visualization tool
- All required LLDB/LLVM libraries

After installation, run `/usr/local/bin/stylusdb` to start using the debugger

#### Install the package for all users (requires admin password)

You can also install the package from the terminal:
```bash
# Download the package (choose the right one for your architecture)
# For Apple Silicon (M1/M2/M3):
curl -L -O https://github.com/walnuthq/stylusdb/releases/download/v0.1.0/StylusDB-0.1.0-macOS-aarch64.pkg

# Or for Intel:
# curl -L -O https://github.com/walnuthq/stylusdb/releases/download/v0.1.0/StylusDB-0.1.0-macOS-x86_64.pkg

# Install the package (requires admin password)
sudo installer -pkg StylusDB-0.1.0-macOS-aarch64.pkg -target /

# Verify installation
/usr/local/bin/stylusdb --version
```

### Prerequisites for `usertrace`

Install Python colorama for pretty-printed traces:
```bash
python3 -m venv myvenv
source ./myvenv/bin/activate
pip3 install colorama
```

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
sudo apt install -y build-essential cmake ninja-build llvm-19-dev liblldb-19-dev lldb-19 swig libzstd-dev
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

##### Build stylusdb

###### macOS
```bash
cd /path/to/stylusdb
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
cd /path/to/stylusdb
mkdir build && cd build
cmake -GNinja .. \
   -DLLVM_DIR=/usr/lib/llvm-19/lib/cmake/llvm \
  -DLLVM_BUILD_ROOT=/usr/lib/llvm-19  \
  -DLLVM_SRC=/usr/include/llvm-19  \
  -DLLVM_TABLEGEN_EXE=/usr/lib/llvm-19/bin/llvm-tblgen \
  -DLLVM_LIB_PATH=/usr/lib/llvm-19/lib/libLLVM-19.so  \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_C_COMPILER=gcc  \
  -DCMAKE_CXX_FLAGS="-Wno-deprecated-declarations"
sudo ninja && sudo ninja install
```

</details>

## Usage

StylusDB is integrated with [cargo-stylus](https://github.com/walnuthq/cargo-stylus) to provide advanced debugging capabilities for Stylus smart contracts on Arbitrum. For complete documentation, see the [Stylus Debugger Guide](https://github.com/walnuthq/cargo-stylus/blob/feature/usertrace/docs/StylusDebugger.md).

### Function Call Tracing with `usertrace`

Trace user function calls in a transaction to understand the execution flow:

```bash
# Basic usage - traces your contract functions
cargo stylus usertrace \
  --tx=0x88b0ad9daa0b701d868a5f9a0132db7c0402178ba44ed8dec4ba76784c7194fd \
  --endpoint=$RPC_URL
```

Output:
```bash
=== STYLUS FUNCTION CALL TREE ===
└─ #1 stylus_hello_world::__stylus_struct_entrypoint::h09ecd85e5c55b994 (lib.rs:33)
    input = size=4
    <anon> = stylus_sdk::host::VM { 0=<unavailable> }
  └─ #2 stylus_hello_world::Counter::increment::h5b9fb276c23de4f4 (lib.rs:64)
      self = 0x000000016fdeaa78
    └─ #3 stylus_hello_world::Counter::set_number::h5bd2c4836637ecb9 (lib.rs:49)
        self = 0x000000016fdeaa78
        new_number = ruint::Uint<256, 4> { limbs=unsigned long[4] { [0]=1, [1]=0, [2]=0, [3]=0 } }
```

#### Advanced Tracing Options

```bash
# Include SDK calls
cargo stylus usertrace --tx <TX_HASH> --endpoint=$RPC_URL --verbose-usertrace

# Trace specific external crates
cargo stylus usertrace --tx <TX_HASH> --endpoint=$RPC_URL \
  --trace-external-usertrace="std,core,other_contract"
```

### Interactive Debugging with `replay`

Use StylusDB for interactive debugging sessions:

```bash
# Basic replay with stylusdb
cargo stylus replay --debugger stylusdb --tx <TX_HASH> --endpoint=$RPC_URL
```

This will:
1. Launch StylusDB
2. Load your contract's debug symbols
3. Stop at breakpoints you can set
4. Allow stepping through code and inspecting variables with contract programming sugar (e.g. pretty-print `uint256` and other well-known types)

#### Multi-Contract Debugging

Debug transactions involving multiple contracts:

```bash
cargo stylus replay --debugger stylusdb --tx <TX_HASH> \
  --contracts 0x123...:./contractA,0x456...:./contractB \
  --endpoint=$RPC_URL
```

In the debugger:
```bash
(stylusdb) stylus-contract breakpoint 0x456... ContractB::some_function
Set breakpoint on ContractB::some_function in contract 0x456... (ID: 1, 1 locations)
(stylusdb) continue
```

#### Mixed Stylus/Solidity Debugging

Debug transactions that call both Stylus and Solidity contracts:

```bash
cargo stylus replay --debugger stylusdb --tx <TX_HASH> \
  --addr-solidity=0xda52b25ddb0e3b9cc393b0690ac62245ac772527 \
  --endpoint=$RPC_URL
```

When execution reaches a Solidity contract:
```bash
(stylusdb)
════════ Solidity Contract Call ════════
Contract: 0xda52b25ddb0e3b9cc393b0690ac62245ac772527
Function selector: 0xd09de08a (increment())
NOTE: This is a Solidity contract - skipping to next contract
```

### Direct StylusDB Usage

You can also use StylusDB directly without cargo-stylus:

```bash
# Start interactive debugger
/usr/local/bin/stylusdb

# Common commands
(stylusdb) target create ./target/release/my_contract.so
(stylusdb) breakpoint set --name user_entrypoint
(stylusdb) run
(stylusdb) continue
(stylusdb) quit
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
