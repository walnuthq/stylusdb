# walnut-dbg
Modern debugger for blockchain transactions.

## Download code

Clone the github repo as follows:

```
git clone https://github.com/walnuthq/walnut-dbg.git
```

## Build

Here are build instructions for both Linux and MacOS.

### MacOS

Download LLVM package with `brew`:

```
$ brew install llvm@19
$ brew install lit
```

First of all, in case of MacOS, we need to download and build LLDB from source (on Linux we do not need this part, since there is `lldb-dev` package available).

```
$ wget https://github.com/llvm/llvm-project/archive/refs/tags/llvmorg-19.1.7.zip
$ unzip llvmorg-19.1.7.zip
$ cd llvm-project-llvmorg-19.1.7/ && mkdir build_lldb && cd build_lldb
$ cmake ../llvm -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS="clang;lldb" -DLLVM_ENABLE_ASSERTIONS=ON -DLLDB_INCLUDE_TESTS=OFF -GNinja
$ ninja
```

Build `walnut-dbg`:

```
$ cd /path/to/walnut-dbg
$ mkdir build && cd build
$ cmake -GNinja .. -DLLVM_DIR=/opt/homebrew/opt/llvm@19/lib/cmake/llvm -DLLVM_BUILD_ROOT=/path/to/llvm-project-llvmorg-19.1.7/build_lldb -DLLVM_SRC=/path/to/llvm-project-llvmorg-19.1.7/ -DLLVM_TABLEGEN_EXE=/opt/homebrew/opt/llvm@19/bin/llvm-tblgen -DCMAKE_CXX_FLAGS="-Wno-deprecated-declarations" -DLLVM_LIB_PATH=/opt/homebrew/opt/llvm@19/lib/libLLVM.dylib
$ sudo ninja && sudo ninja install
[0/1] Install the project...
-- Installing: /usr/local/lib/libFunctionCallTrace.a
-- Installing: /usr/local/lib/libFunctionCallTracePlugin.dylib
-- Installing: /usr/local/bin/walnut-dbg
```

NOTE: Replace `/path/to/llvm-project-llvmorg-19.1.7/build_lldb` with real path!

### Linux

TODO

## Run the tool

```
$ /usr/local/bin/walnut-dbg
(walnut-dbg) q
```
