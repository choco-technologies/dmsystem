# dmsystem

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

dmsystem DMOD application module.

## Description

TODO: describe what this module does.

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

## Usage

This application module can be loaded and executed using the DMOD loader:

```bash
dmod_loader /path/to/dmsystem.dmf
```

## Documentation

See the `docs/` directory:

- **[api-reference.md](docs/api-reference.md)** - Command-line usage

View documentation using `dmf-man dmsystem`.

## Project Structure

```
dmsystem/
├── docs/              # Documentation (markdown format)
├── src/
│   └── dmsystem.c
├── tests/
│   ├── CMakeLists.txt
│   └── dmsystem_test.c
├── CMakeLists.txt
├── Makefile
├── dmsystem.dmr
└── manifest.dmm
```

## Author

John Doe

## License

MIT
