from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps
import os
import runpy


required_conan_version = ">=2.28"


class CodeRoastIpcNamespaceTestsConan(ConanFile):
    name = "coderoast_ipc_namespace_tests"
    version = "1.10.8"
    # DN-142.D29 (ix): the core's segment arms that mount a private tmpfs in a fresh user, pid and
    # mount namespace, which no Landlock domain allows. A test-only package declared `unconfined:` in
    # packages.yml and built against the STORED coderoast_ipc_core, so the core's own create stays
    # confined and reusable. It ships nothing: its create builds the suite and runs it.
    package_type = "application"
    license = "Apache-2.0"
    description = ("The mount-namespace segment tests of coderoast_ipc_core, run unconfined against the "
                   "stored core (DN-142.D29 (ix)). Never published.")
    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "CMakeLists.txt", "tests/*"
    requires = "coderoast_ipc_core/1.10.8"

    def export_sources(self):
        # The export is the TRACKED files under exports_sources, never the disk's (DN-142.D4 (b)):
        # malf's helper, staged in the conan home and named by global.conf, drops every file git
        # does not track. Without it the export fails; it never falls back to the disk silently.
        runpy.run_path(self.conf.get("user.malf:recipe_exports"))["narrow_to_tracked"](self)

    def layout(self):
        # Keyed editable build dir, as every package of this repository keys it (malf's env, or the
        # profile's [conf] under a raw `conan create --profile X`).
        build_dir = (os.environ.get("MALF_EDITABLE_BUILD_DIR")
                     or self.conf.get("user.malf:editable_build_dir", default="build"))
        self.cpp.build.libdirs = [build_dir]
        self.cpp.build.builddirs = [build_dir]

    def build_requirements(self):
        self.test_requires("gtest/1.17.0")

    def generate(self):
        tc = CMakeToolchain(self)
        tc.generator = "Ninja"
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        runpy.run_path(self.conf.get("user.malf:recipe_tests"))["run_tests"](self)

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        # A test-only package: nothing to link or run downstream.
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
        self.cpp_info.includedirs = []
