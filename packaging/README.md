# Roxal .deb Packaging

Build a `.deb` package for Roxal from an existing cmake build.

## Prerequisites

The build machine needs:
- `strip`, `patchelf`, `dpkg-deb` (install with `sudo apt install patchelf`)
- A completed cmake Release build of roxal

## Building the .deb

First, configure and build roxal as a Release build with the desired modules:

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DROXAL_ENABLE_FILEIO=ON \
  -DROXAL_ENABLE_REGEX=ON \
  -DROXAL_ENABLE_SOCKET=ON \
  -DROXAL_ENABLE_GRPC=ON \
  -DROXAL_ENABLE_DDS=ON \
  -DROXAL_ENABLE_AI_NN=ON

cmake --build build -j$(nproc)
```

Then build the `.deb`:

```bash
./packaging/build-deb.sh
```

The output `.deb` will be placed in the project root directory.

### Options

| Flag | Default | Description |
|---|---|---|
| `--distro` | `noble` | Ubuntu codename (selects the dependency list from `templates/deps-<distro>.txt`) |
| `--build-dir` | `build` | Path to the cmake build directory |
| `--antlr4-lib-dir` | *(none)* | Path to ANTLR4 runtime shared libraries (bundled into the package) |
| `--dds-lib-dir` | `/usr/share/lib` | Path to CycloneDDS shared libraries (bundled into the package) |
| `--ort-lib-dir` | `deps/onnxruntime/lib` | Path to ONNX Runtime shared libraries (bundled into the package) |
| `--output-dir` | `.` (project root) | Where to place the resulting `.deb` |

## Installing

```bash
sudo apt install ./roxal_<version>_amd64.deb
```

`apt` will automatically fetch and install all dependencies from the Ubuntu repos.

## Removing

```bash
sudo apt remove roxal
```

## Version string

The package version is assembled from several sources:

```
0.8.0~pre+20260218.g66e36b0~noble
^     ^   ^        ^        ^
|     |   |        |        └── Ubuntu codename (--distro)
|     |   |        └── short git hash
|     |   └── UTC build date
|     └── prerelease label from CMakeLists.txt (ROXAL_PRERELEASE)
└── version from CMakeLists.txt (project VERSION)
```

## Building with Docker (recommended)

`docker-build.sh` builds the `.deb` inside Docker — no host dependencies needed
beyond Docker itself. It handles all build-time dependencies, cross-compilation,
and packaging automatically.

```bash
# Build for the host architecture (default: arm64)
./packaging/docker-build.sh

# Build for a specific platform
./packaging/docker-build.sh --platform linux/amd64
./packaging/docker-build.sh --platform linux/arm64
```

The resulting `.deb` is placed in the project root directory.

**Notes:**
- The amd64 build includes GPU-enabled ONNX Runtime (CUDA + TensorRT providers).
- The arm64 build uses CPU-only ONNX Runtime (no pre-built GPU variant available).
- Cross-architecture builds use QEMU emulation and build with `-j1` for stability.
  The Dockerfile includes retry logic to handle intermittent QEMU segfaults.
- Native builds use half the available CPU cores.

## Testing with Docker

Test the `.deb` on a clean Ubuntu 24.04 system using Docker:

```bash
# Start a container with the project mounted
docker run --rm -it -v "$PWD":/pkg ubuntu:24.04 bash
```

Inside the container:

```bash
# Install
apt-get update
apt-get install -y /pkg/roxal_*noble*.deb

# Verify
roxal --version
roxal -e "print('hello from the .deb')"

# Test module imports
printf "import math.*\nprint(sqrt(144))\n" | roxal
printf "import fileio\nprint(fileio.file_exists('/etc/hostname'))\n" | roxal
printf "import regex\nvar re = regex.Regex('[0-9]+')\nprint(re.test('abc123'))\n" | roxal
printf "import socket\nvar s = socket.tcp()\nprint(s)\ns.close()\n" | roxal
printf "import ai.nn\nprint('ai.nn loaded')\n" | roxal
```

## Adding support for another Ubuntu release

1. Create `templates/deps-<codename>.txt` with the correct package names and versions for that release (the library SONAMEs differ between Ubuntu releases).
2. Rebuild roxal on that release (the binary must be compiled against matching libraries).
3. Run `./packaging/build-deb.sh --distro <codename>`.

## What's in the package

| Path | Contents |
|---|---|
| `/usr/bin/roxal` | Stripped binary |
| `/usr/share/roxal/*.rox` | Standard library modules (sys, math, fileio, regex, socket, dds, ai.nn) |
| `/usr/lib/roxal/libddsc*` | Bundled CycloneDDS libraries |
| `/usr/lib/roxal/libonnxruntime*` | Bundled ONNX Runtime libraries (CPU; GPU if CUDA provider included) |
| `/etc/ld.so.conf.d/roxal.conf` | Tells the dynamic linker about `/usr/lib/roxal` |
