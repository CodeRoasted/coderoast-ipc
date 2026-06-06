from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps
from conan.tools.files import save
import os


required_conan_version = ">=2.28"


class CodeRoastIpcProducerConan(ConanFile):
    name = "coderoast_ipc_producer"
    version = "1.5.1"
    # 1.5.1 unwrap: PURE named module (coderoast.ipc.producer) — header-only api/ surface now
    # lives in the module interface; the textual api/ headers are gone. static-library; no header surface.
    package_type = "static-library"
    license = "Apache-2.0"
    description = "Producer-side helpers for coderoast-ipc (frame building, sequence tracking, shard distribution)."
    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "CMakeLists.txt", "api/*", "tests/*"
    requires = "coderoast_ipc_core/1.5.1"

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

        # Generate CMakeDeps for requires (core), but skip for test_requires (gtest)
        deps = CMakeDeps(self)
        deps.generate()

        # For test builds, inject gtest paths directly to avoid CMakeDeps issues
        if hasattr(self, "dependencies") and self.dependencies:
            gtest_dep = self.dependencies.get("gtest", None)
            if gtest_dep:
                gtest_include = gtest_dep.cpp_info.includedirs[0]
                gtest_libs = gtest_dep.cpp_info.libdirs[0] if gtest_dep.cpp_info.libdirs else None
                
                if gtest_include and gtest_libs:
                    # Create a CMake script to pass these paths
                    paths_content = f'''
set(GTEST_INCLUDE_DIR "{gtest_include}")
set(GTEST_LIB_DIR "{gtest_libs}")
'''
                    save(self, os.path.join(self.generators_folder, "gtest_paths.cmake"), paths_content)

    def build(self):
        # The module wrapper (coderoast.ipc.producer) is a compiled .cppm.o + its
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
        self.cpp_info.set_property("cmake_file_name", "coderoast_ipc_producer")
        self.cpp_info.set_property("cmake_target_name", "coderoast::ipc::producer")
        self.cpp_info.bindirs = []
        # Cross-package C++ modules (§10.7): defer to the package's OWN cmake config
        # (carries FILE_SET CXX_MODULES). The new CMakeConfigDeps generator derives
        # <pkg>_DIR (find_package's config hint) from builddirs[0], so it MUST hold the
        # native config in the CURRENT consumption mode, else find_package(<pkg>) fails:
        #   - editable    → export(EXPORT) wrote it into MALF_EDITABLE_BUILD_DIR
        #   - conan create → install() shipped it under lib/cmake/coderoast_ipc_producer
        self.cpp_info.set_property("cmake_find_mode", "none")
        malf_editable_build_dir = os.environ.get("MALF_EDITABLE_BUILD_DIR")
        if malf_editable_build_dir:
            self.cpp_info.builddirs = [malf_editable_build_dir, "lib/cmake/coderoast_ipc_producer"]
        else:
            self.cpp_info.builddirs = ["lib/cmake/coderoast_ipc_producer"]
