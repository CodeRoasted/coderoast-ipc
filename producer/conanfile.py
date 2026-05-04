from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain


class CodeRoastIpcProducerConan(ConanFile):
    name = "coderoast_ipc_producer"
    version = "1.0.0"
    package_type = "header-library"
    license = "Apache-2.0"
    description = "Producer-side helpers for coderoast-ipc (frame building, sequence tracking, shard distribution)."
    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "CMakeLists.txt", "api/*", "tests/*"
    requires = "coderoast_ipc_core/1.2.8"

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

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "coderoast_ipc_producer")
        self.cpp_info.set_property("cmake_target_name", "coderoast::ipc::producer")
        self.cpp_info.requires = ["coderoast_ipc_core::coderoast_ipc_core"]
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
