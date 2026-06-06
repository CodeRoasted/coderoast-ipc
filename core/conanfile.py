from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain
from conan.tools.files import save
import os


required_conan_version = ">=2.28"


class CodeRoastIpcCoreConan(ConanFile):
    name = "coderoast_ipc_core"
    version = "1.5.1"
    # 1.5.1 unwrap: PURE named module (coderoast.ipc.core) — the former header-only
    # api/ surface now lives in the module interface; the textual api/ headers are gone.
    # One .cppm interface + one textual impl unit (the errno/POSIX syscalls, §11.9) →
    # static-library. No header surface ships.
    package_type = "static-library"
    license = "Apache-2.0"
    description = "Core transport primitives for coderoast-ipc (SPSC channel, frame types)."
    settings = "os", "arch", "compiler", "build_type"

    exports_sources = "CMakeLists.txt", "api/*", "tests/*", "benchmarks/*"

    def layout(self):
        build_dir = os.environ.get("MALF_EDITABLE_BUILD_DIR", "build")
        self.cpp.build.libdirs = [build_dir]
        # Editable: the build-tree export()'d coderoast_ipc_core-config.cmake (carrying the
        # FILE_SET CXX_MODULES) lives in the build dir → consumers find it there (§10.9).
        self.cpp.build.builddirs = [build_dir]

    def build_requirements(self):
        self.test_requires("gtest/1.17.0")
        self.test_requires("benchmark/1.9.5")

    def generate(self):
        tc = CMakeToolchain(self)
        tc.generator = "Ninja"
        tc.generate()

        # For test builds, inject gtest paths directly to avoid CMakeDeps issues
        if hasattr(self, "dependencies") and self.dependencies:
            gtest_dep = self.dependencies.get("gtest", None)
            if gtest_dep:
                gtest_include = gtest_dep.cpp_info.includedirs[0]
                gtest_libs = gtest_dep.cpp_info.libdirs[0] if gtest_dep.cpp_info.libdirs else None
                
                if gtest_include and gtest_libs:
                    paths_content = f'''
                        set(GTEST_INCLUDE_DIR "{gtest_include}")
                        set(GTEST_LIB_DIR "{gtest_libs}")
                        '''
                    save(self, os.path.join(self.generators_folder, "gtest_paths.cmake"), paths_content)

            benchmark_dep = self.dependencies.get("benchmark", None)
            if benchmark_dep:
                benchmark_include = benchmark_dep.cpp_info.includedirs[0]
                benchmark_libs = benchmark_dep.cpp_info.libdirs[0] if benchmark_dep.cpp_info.libdirs else None
                
                if benchmark_include and benchmark_libs:
                    paths_content = f'''
                        set(BENCHMARK_INCLUDE_DIR "{benchmark_include}")
                        set(BENCHMARK_LIB_DIR "{benchmark_libs}")
                        '''
                    save(self, os.path.join(self.generators_folder, "benchmark_paths.cmake"), paths_content)

    def build(self):
        # coderoast.ipc.core is a compiled module (.cppm.o + impl .o) + its build-tree
        # export()'d config — configure+build emits both. No header surface.
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        # install(EXPORT) + the -config.cmake ship the FILE_SET CXX_MODULES + archive (§10.7);
        # no install(DIRECTORY api/) (textual headers retired). CMakeLists is the source of truth.
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "coderoast_ipc_core")
        self.cpp_info.set_property("cmake_target_name", "coderoast::ipc::core")
        self.cpp_info.bindirs = []
        # Cross-package C++ modules (§10.7): defer to the package's OWN cmake config
        # (it carries FILE_SET CXX_MODULES; conan's generator does not emit it). The new
        # CMakeConfigDeps generator derives <pkg>_DIR (find_package's config hint) from
        # builddirs[0], so it MUST hold the native config in the CURRENT consumption mode,
        # else a consumer's find_package(<pkg>) fails (the conan-create defect):
        #   - editable    → export(EXPORT) wrote it into MALF_EDITABLE_BUILD_DIR
        #   - conan create → install() shipped it under lib/cmake/coderoast_ipc_core
        # core is std-only → no find_dependency.
        self.cpp_info.set_property("cmake_find_mode", "none")
        malf_editable_build_dir = os.environ.get("MALF_EDITABLE_BUILD_DIR")
        if malf_editable_build_dir:
            self.cpp_info.builddirs = [malf_editable_build_dir, "lib/cmake/coderoast_ipc_core"]
        else:
            self.cpp_info.builddirs = ["lib/cmake/coderoast_ipc_core"]
