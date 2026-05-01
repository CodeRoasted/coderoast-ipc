# coderoast-ipc

`coderoast-ipc` owns the `coderoast_ipc` Conan package: shared-memory channels, SPSC queues, and frame types used by local CodeRoast pipelines.

The package is intentionally small and product-neutral. LogCraft produces frames, InSight consumes frames, and server-side code may depend on the package transitively through LogCraft core, but this repository owns the IPC ABI and release cadence.

## Build

Builds into the repo-local `.conan2` cache:

```bash
conan create . \
  --profile:host=.conan2/profiles/linux-gcc13-release \
  --profile:build=.conan2/profiles/linux-gcc13-release \
  --build=missing \
  --build-test=missing
```

### Export to the shared stable cache

Sibling repos (`logcraft`, `insight`, `coderoast-server`) depend on `coderoast_ipc` at build time.
They resolve it from `/opt/coderoast/conan-stable`, the shared local cache populated by this command:

```bash
CONAN_HOME=/opt/coderoast/conan-stable conan create . \
  --profile:host=linux-gcc13-release \
  --profile:build=linux-gcc13-release \
  --build=missing
```

Run this after bumping the version or changing the ABI. The `malf build` alias used by sibling repos
picks up the new version automatically on the next run (it bootstraps missing packages from the stable cache).

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

Consumers restore the tarball with `conan cache restore` before building packages that require `coderoast_ipc`.