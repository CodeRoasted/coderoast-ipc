# coderoast-ipc

`coderoast-ipc` owns the `coderoast_ipc/0.1.0` Conan package: shared-memory channels, SPSC queues, and frame types used by local CodeRoast pipelines.

The package is intentionally small and product-neutral. LogCraft produces frames, InSight consumes frames, and server-side code may depend on the package transitively through LogCraft core, but this repository owns the IPC ABI and release cadence.

## Build

```bash
conan create . \
  --profile:host=.conan2/profiles/linux-gcc13-release \
  --profile:build=.conan2/profiles/linux-gcc13-release \
  --build=missing \
  --build-test=missing
```

For local CMake iteration:

```bash
conan install . --output-folder=build --build=missing \
  --profile:host=.conan2/profiles/linux-gcc13-release \
  --profile:build=.conan2/profiles/linux-gcc13-release \
  -s build_type=Debug
cmake --preset conan-debug -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Release

Tags use `v0.1.0` style semver. The release workflow builds `coderoast_ipc`, runs its tests, exports `coderoast_ipc-<version>.tgz` with `conan cache save`, and attaches the tarball to the GitHub release.

Consumers restore the tarball with `conan cache restore` before building packages that require `coderoast_ipc/0.1.0`.