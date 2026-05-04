from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain


class CodeRoastIpcCoreConan(ConanFile):
    name = "coderoast_ipc_core"
    version = "1.2.8"
    package_type = "header-library"
    license = "Apache-2.0"
    description = "Low-level SPSC shared-memory IPC transport primitives (channel, frame types)."
    settings = "os", "arch", "compiler", "build_type"
    exports_sources = "CMakeLists.txt", "api/*", "tests/*", "benchmarks/*"

    def build_requirements(self):
        self.test_requires("gtest/1.17.0")
        self.test_requires("benchmark/1.8.3")

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
        self.cpp_info.set_property("cmake_file_name", "coderoast_ipc_core")
        self.cpp_info.set_property("cmake_target_name", "coderoast::ipc::core")
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
