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
  -DROXAL_ENABLE_ONNX=OFF \
  -DROXAL_ENABLE_AI_NN=OFF

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
| `--dds-lib-dir` | `/usr/share/lib` | Path to CycloneDDS shared libraries (bundled into the package) |
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

## Testing with Docker

Test the `.deb` on a clean Ubuntu 24.04 system using Docker:

```bash
# Find the .deb filename
DEB=$(ls roxal_*noble*.deb | head -1)

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
```

## Adding support for another Ubuntu release

1. Create `templates/deps-<codename>.txt` with the correct package names and versions for that release (the library SONAMEs differ between Ubuntu releases).
2. Rebuild roxal on that release (the binary must be compiled against matching libraries).
3. Run `./packaging/build-deb.sh --distro <codename>`.

## What's in the package

| Path | Contents |
|---|---|
| `/usr/bin/roxal` | Stripped binary |
| `/usr/share/roxal/*.rox` | Standard library modules (sys, math, fileio, regex, socket, dds) |
| `/usr/lib/roxal/*.so*` | Bundled CycloneDDS libraries (not available from Ubuntu repos) |
| `/etc/ld.so.conf.d/roxal.conf` | Tells the dynamic linker about `/usr/lib/roxal` |
