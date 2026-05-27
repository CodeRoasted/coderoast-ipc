from conan import ConanFile
from conan.tools.cmake import CMakeToolchain
from conan.tools.files import copy, save
import os


required_conan_version = ">=2.28"


class CodeRoastIpcCoreConan(ConanFile):
    name = "coderoast_ipc_core"
    version = "1.2.1"
    package_type = "header-library"
    license = "Apache-2.0"
    description = "Core transport primitives for coderoast-ipc (SPSC channel, frame types)."
    settings = "os", "arch", "compiler", "build_type"

    exports_sources = "CMakeLists.txt", "api/*", "tests/*", "benchmarks/*"

    def layout(self):
        self.cpp.source.includedirs = ["api"]

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
        # Header-only library - nothing to build
        pass

    def package(self):
        # Copy headers to package
        copy(self, "*.hpp", src=self.source_folder + "/api",
             dst=self.package_folder + "/include", keep_path=True)

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "coderoast_ipc_core")
        self.cpp_info.set_property("cmake_target_name", "coderoast::ipc::core")
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
