# Build and package checks

Frond 0.5.0 targets Debian 13 (trixie) with KDE Plasma. Build tools are listed in
[the README](../README.md). Packaging also needs `dpkg-dev` and `binutils`.
The installed package uses `mtools` for SD exchange and `libslirp0` for virtual
Wi-Fi; the latter must be found from the core's ELF dependencies. Qt's platform,
Wayland and SVG plugins are runtime dependencies too.

## Reconstructing the core

`vendor/cloudpilot` is pinned to
`064d5d578bcd1f9072de384d05aa217f83db2f7f`. Preparation checks that its HEAD,
Frond's gitlink and `tools/prepare.py` agree. It archives that commit, rather
than copying the vendor working tree, then applies `patches/*.patch` in lexical
order. Each patch needs a unique four-digit prefix; gaps are allowed.
`tools/debug/*.patch` is optional instrumentation and is never applied by the
normal build.

`build/source/.prepared` records the pin and the preparation script and patch
hashes. Every Make invocation checks these inputs, including added, removed or
changed patches. It preserves local edits in generated source. If inputs have
changed, preparation stops and leaves the whole existing tree alone. Export
any local source changes into a patch, then reconstruct in a fresh worktree;
there is no automatic clean or source replacement.

```sh
make test-build-tools check-reconstruction
```

The first target tests staging safety and package policy with synthetic data.
It compiles dummy ELF executables and creates disposable test packages in a
temporary directory; it does not package Frond. The second reconstructs the core
twice in independent temporary directories, compares file hashes and modes,
and checks each reconstructed input against `build/source` if it exists.
Neither target needs a ROM, Palm app or saved session. A matching preparation
stamp alone does not prove that generated source is unmodified; use the
reconstruction check before assessing a build.

For an independent checkout while retaining your current source and sessions:

```sh
git worktree add --detach ../frond-validation HEAD
git -C ../frond-validation submodule update --init vendor/cloudpilot
cd ../frond-validation
make -j4 test
QT_QPA_PLATFORM=offscreen make test-desktop
make check-reconstruction
```

A worktree starts from committed files. Commit any patches you intend to
validate first. On hosts with the shared `heavy` wrapper, use
`heavy build make -j4 test` and `heavy build make test-desktop`. Set
`BUILD_JOBS=2` to limit the desktop build's parallelism independently.

## Checking a local Debian package

When you choose to package a local build, `make deb` checks both binary versions,
strips copies in a new staging directory, discovers ELF dependencies with
`dpkg-shlibdeps`, and verifies an exact installed-file inventory before and after
`dpkg-deb`. Only the two executables, desktop integration and listed notices and
guides are included. No directory of firmware, sessions, apps, SD images, logs or
build results is copied. Links and unexpected files are rejected.

The package uses `SOURCE_DATE_EPOCH`, defaulting to the source commit time,
normalizes staged timestamps and modes, uses a gzip header without a source
filename, and fixes Debian compression settings. Synthetic tests compare package
bytes after changing input mtimes and seed private-looking files beside inputs
to check their exclusion. This verifies repeatable packaging of identical
binaries with the same toolchain; it does not establish bit-identical compiler
output across toolchain versions or checkout paths.

An existing package can be checked without installing it:

```sh
python3 tools/check-package.py out/frond_0.5.0-1_amd64.deb
```

The validator checks the version, architecture, required runtime dependencies,
root ownership, modes and complete installed-file inventory. It cannot identify
personal data deliberately embedded in an otherwise approved executable or
document, so build only from reviewed source and keep private inputs outside
that inventory.

## Automated coverage and limits

`.github/workflows/validate.yml` currently runs manually with `workflow_dispatch`
in Debian trixie. It installs `mtools`, `libslirp-dev` and `gdb-multiarch`, initializes the pinned
submodule, runs independent reconstruction and synthetic packaging checks, then
builds and runs the firmware-free core and Qt desktop tests. It checks source
again after testing. It does not create Frond release packages or upload files,
tag commits, publish releases or push changes.

The Qt tests run offscreen. They do not replace a manual Plasma/Wayland test,
installation of a real package in a fresh Trixie environment, or Palm firmware
acceptance and audio checks. Those remain separate checks using private inputs.
