# Building the extension

## Build Architecture

For the DuckDB extension to call the Rust code a tool called `cbindgen` is used to write the C++ headers for the exposed Rust interface.

The headers can be updated by running `make rust_binding_headers`.

## Building on MacOS X

Example setup + build steps for macOS users:

```sh
# Remove rust if previously installed via brew
brew uninstall rust

# Install rustup + cbindgen
# (use rustup to switch versions of Rust without extra fuss)
brew install cbindgen rustup

rustup toolchain install stable

# Initialize rustup
# Zsh users: customize installation, answer n to "Modify PATH variable?",
# and continue with defaults for everything else
rustup-init

# OPTIONAL step for zsh users: add rust + cargo env setup to zshrc:
echo '. "$HOME/.cargo/env"' >> ~/.zshrc

# Use rustc stable version by default
rustup default stable

# Build headers
make rust_binding_headers

GEN=ninja make
```

## Build steps

Now to build the extension, run:
```sh
make
```
The main binaries that will be built are:
```sh
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/lindel/lindel.duckdb_extension
```
- `duckdb` is the binary for the duckdb shell with the extension code automatically loaded.
- `unittest` is the test runner of duckdb. Again, the extension is already linked into the binary.
- `lindel.duckdb_extension` is the loadable binary as it would be distributed.

## Running the extension

To run the extension code, simply start the shell with `./build/release/duckdb`.

Now we can use the features from the extension directly in DuckDB.

```
D select hilbert_encode([1.0, 5.0, 6.0]::float[3]) as hilbert;
┌──────────────────────────────┐
│           hilbert            │
│           uint128            │
├──────────────────────────────┤
│ 8002395622101954260073409974 │
└──────────────────────────────┘
```

## Running the tests

Different tests can be created for DuckDB extensions. The primary way of testing DuckDB extensions should be the SQL tests in `./test/sql`. These SQL tests can be run using:
```sh
make test
```

## Installing the deployed binaries

To install your extension binaries from S3, you will need to do two things. Firstly, DuckDB should be launched with the
`allow_unsigned_extensions` option set to true. How to set this will depend on the client you're using. Some examples:

CLI:
```shell
duckdb -unsigned
```

Python:
```python
con = duckdb.connect(':memory:', config={'allow_unsigned_extensions' : 'true'})
```

NodeJS:
```js
db = new duckdb.Database(':memory:', {"allow_unsigned_extensions": "true"});
```

Secondly, you will need to set the repository endpoint in DuckDB to the HTTP url of your bucket + version of the extension
you want to install. To do this run the following SQL query in DuckDB:
```sql
SET custom_extension_repository='bucket.s3.us-east-1.amazonaws.com/lindel/latest';
```
Note that the `/latest` path will allow you to install the latest extension version available for your current version of
DuckDB. To specify a specific version, you can pass the version instead.

After running these steps, you can install and load your extension using the regular INSTALL/LOAD commands in DuckDB:
```sql
INSTALL lindel
LOAD lindel
```
