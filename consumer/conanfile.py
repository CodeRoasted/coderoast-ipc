from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps
from conan.tools.files import copy, save
import os


class CodeRoastIpcConsumerConan(ConanFile):
    name = "coderoast_ipc_consumer"
    version = "1.0.1"
    package_type = "header-library"
    license = "Apache-2.0"
    description = "Ordered stream consumer adapter for coderoast-ipc (fan-in from sharded channels, sequence gap handling)."
    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "CMakeLists.txt", "api/*", "tests/*"
    requires = "coderoast_ipc_core/1.0.1"

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
        # Header-only library - nothing to build
        pass

    def package(self):
        # Copy headers to package
        copy(self, "*.hpp", src=self.source_folder + "/api",
             dst=self.package_folder + "/include", keep_path=True)

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "coderoast_ipc_consumer")
        self.cpp_info.set_property("cmake_target_name", "coderoast::ipc::consumer")
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
