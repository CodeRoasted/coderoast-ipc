from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps
import os


required_conan_version = ">=2.28"


class CodeRoastIpcConsumerConan(ConanFile):
    name = "coderoast_ipc_consumer"
    version = "1.6.5"
    # 1.5.1 unwrap: PURE named module (coderoast.ipc.consumer) — header-only api/ surface now
    # lives in the module interface; the textual api/ headers are gone. static-library; no header surface.
    package_type = "static-library"
    license = "Apache-2.0"
    description = "Ordered stream consumer adapter for coderoast-ipc (fan-in from sharded channels, sequence gap handling)."
    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "CMakeLists.txt", "api/*", "tests/*"
    requires = "coderoast_ipc_core/1.6.5"

    def layout(self):
        # Keyed editable build dir: malf sets the env (all profiles incl. sanitizer); a RAW
        # `conan create --profile X` instead reads it from the profile [conf] → a consumer under
        # ANY profile links THIS dep's matching-profile build, not the libc++-default build/
        # ([[malf-build-type-isolation]] keying gap).
        build_dir = (os.environ.get("MALF_EDITABLE_BUILD_DIR")
                     or self.conf.get("user.malf:editable_build_dir", default="build"))
        self.cpp.build.libdirs = [build_dir]
        # Editable: the build-tree export()'d -config.cmake (carrying FILE_SET
        # CXX_MODULES) lives in the build dir → consumers find it there (§10.9).
        self.cpp.build.builddirs = [build_dir]

    def build_requirements(self):
        self.test_requires("gtest/1.17.0")

    def generate(self):
        tc = CMakeToolchain(self)
        tc.generator = "Ninja"
        tc.generate()

        # CMakeConfigDeps (the workspace default generator, §10.12) emits configs for
        # the core dep AND the gtest test_requires — the old manual conan-cache path
        # injection is retired.
        deps = CMakeDeps(self)
        deps.generate()


    def build(self):
        # The module wrapper (coderoast.ipc.consumer) is a compiled .cppm.o + its
        # build-tree export()'d config — configure+build emits both.
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        # install(EXPORT) + the -config.cmake ship the FILE_SET CXX_MODULES + archive (§10.7);
        # no install(DIRECTORY api/) (textual headers retired). CMakeLists is the source of truth.
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "coderoast_ipc_consumer")
        self.cpp_info.set_property("cmake_target_name", "coderoast::ipc::consumer")
        self.cpp_info.bindirs = []
        # Cross-package C++ modules (§10.7): defer to the package's OWN cmake config
        # (carries FILE_SET CXX_MODULES). The new CMakeConfigDeps generator derives
        # <pkg>_DIR (find_package's config hint) from builddirs[0], so it MUST hold the
        # native config in the CURRENT consumption mode, else find_package(<pkg>) fails:
        #   - editable    → export(EXPORT) wrote it into MALF_EDITABLE_BUILD_DIR
        #   - conan create → install() shipped it under lib/cmake/coderoast_ipc_consumer
        self.cpp_info.set_property("cmake_find_mode", "none")
        malf_editable_build_dir = os.environ.get("MALF_EDITABLE_BUILD_DIR")
        if malf_editable_build_dir:
            self.cpp_info.builddirs = [malf_editable_build_dir, "lib/cmake/coderoast_ipc_consumer"]
        else:
            self.cpp_info.builddirs = ["lib/cmake/coderoast_ipc_consumer"]
