# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Lindel is a DuckDB extension that provides Hilbert and Morton (Z-order) encoding/decoding functions for multi-dimensional data linearization. It's a DuckDB Community Extension that enables sorting multi-dimensional data using space-filling curves for improved query performance on spatial or multi-field data.

## Build Commands

```bash
# Build release version
GEN=ninja make release

# Build debug version
GEN=ninja make debug

# Run all tests (requires building first)
make test              # runs against release build
make test_debug        # runs against debug build

# Format code
make format

```

All extension functions should be documented inside of DuckDB with CreateScalarFunctionInfo or CreateAggregateFunctionInfo or the appropriate type for the function.  This documentation of the function should include examples, parameter types and parameter names.  The function should be categorized.

When making changes the version should always be updated to the current date plus an ordinal counter in the form of YYYYMMDDCC.


### macOS Setup Prerequisites

```bash
brew install cbindgen rustup
rustup toolchain install stable
rustup-init
rustup default stable
```

## Build Outputs

- `./build/release/duckdb` - DuckDB shell with extension loaded
- `./build/release/test/unittest` - Test runner
- `./build/release/extension/lindel/lindel.duckdb_extension` - Loadable extension binary

## Architecture

### C++/Rust Hybrid

The extension uses a C++/Rust hybrid architecture:

1. **C++ Layer** (`src/lindel_extension.cpp`): DuckDB extension interface
   - Registers `hilbert_encode`, `hilbert_decode`, `morton_encode`, `morton_decode` scalar functions
   - Handles type validation, bind functions, and DuckDB vector operations
   - Uses `lindelEncodingBindData` to track encoding type (0=Hilbert, 1=Morton)

2. **Rust Layer** (`duckdb_lindel_rust/`): Core encoding/decoding logic
   - Wraps the `lindel` Rust crate for Hilbert/Morton curve algorithms
   - Exposes C FFI functions via `cbindgen`-generated headers (`src/include/rust.h`)
   - Built as a static library linked into the extension

### FFI Interface

Rust functions exposed to C++:
- `hilbert_encode_u8_var`, `hilbert_encode_u16_var`, `hilbert_encode_u32_var`, `hilbert_encode_u64_var`
- `morton_encode_u8_var`, `morton_encode_u16_var`, `morton_encode_u32_var`, `morton_encode_u64_var`
- `perform_decode` - unified decode function for both encoding types

### Build System

- Uses CMake with DuckDB's extension build infrastructure
- Corrosion integrates Rust/Cargo into CMake build
- Configuration in `extension_config.cmake`
- Inherits from `extension-ci-tools/makefiles/duckdb_extension.Makefile`

## Testing

SQL tests are in `test/sql/lindel.test`. Run with:
```bash
make test                    # Release tests
make test_debug              # Debug tests
./build/release/test/unittest "test/*"  # Direct unittest runner
```

## Key Files

- `src/lindel_extension.cpp` - Main extension code with all DuckDB function implementations
- `duckdb_lindel_rust/src/lib.rs` - Rust encoding/decoding implementations
- `src/include/rust.h` - Auto-generated C++ headers for Rust FFI (regenerate with `make rust_binding_headers`)
- `duckdb_lindel_rust/cbindgen.toml` - Configuration for header generation
