from conan import ConanFile
from conan.tools.files import copy
import os


required_conan_version = ">=2.28"


class CodeRoastIpcConan(ConanFile):
    """
    Meta-package for CodeRoast IPC: orchestrates building and packaging all three sub-packages.

    This is a convenience package that:
      1. Exports all source code for the three sub-packages (core, consumer, producer)
      2. Is used by CI/release workflows as the entry point
      3. Triggers separate builds of coderoast_ipc_core, coderoast_ipc_consumer, coderoast_ipc_producer

    Downstream consumers should depend on the specific packages they need:
      - requires = "coderoast_ipc_core/1.5.0"       # Just transport layer
      - requires = "coderoast_ipc_consumer/1.5.0"   # Consumer adapter + core
      - requires = "coderoast_ipc_producer/1.5.0"   # Producer helper + core

    This meta-package (coderoast_ipc) is only used internally by CI/release workflows
    to coordinate multi-package builds. Downstream packages should use the specific sub-packages.
    """

    name = "coderoast_ipc"
    version = "1.5.2"
    package_type = "header-library"
    license = "Apache-2.0"
    description = (
        "Meta-package for CodeRoast IPC. Coordinates building of core, consumer, and producer packages. "
        "Use specific sub-packages (coderoast_ipc_core, etc.) in your requires."
    )
    settings = "os", "arch", "compiler", "build_type"

    # Export all sub-package sources (the actual packages Conan will extract)
    exports_sources = (
        "core/CMakeLists.txt", "core/conanfile.py", "core/api/*", "core/tests/*", "core/benchmarks/*",
        "consumer/CMakeLists.txt", "consumer/conanfile.py", "consumer/api/*", "consumer/tests/*",
        "producer/CMakeLists.txt", "producer/conanfile.py", "producer/api/*", "producer/tests/*",
    )

    def set_version(self):
        """Ensure version consistency across all sub-packages."""
        # Optionally read core version if you want to sync versions
        import ast
        import pathlib
        try:
            recipe_folder = self.recipe_folder or "."
            core_path = pathlib.Path(recipe_folder) / "core" / "conanfile.py"
            if core_path.exists():
                tree = ast.parse(core_path.read_text())
                for node in ast.walk(tree):
                    if isinstance(node, ast.Assign):
                        for target in node.targets:
                            if isinstance(target, ast.Name) and target.id == "version":
                                if isinstance(node.value, ast.Constant):
                                    self.version = node.value.value
        except Exception:
            pass

    def export_sources(self):
        """Ensure sub-package sources are available for Conan to discover."""
        # The exports_sources above handles this, but this method can be used for
        # additional validation if needed.
        pass

    def build(self):
        """Nothing to build; this is a meta-package."""
        pass

    def package(self):
        """Copy aggregated headers from all sub-packages for reference."""
        # This is optional; mainly for documentation. Downstream packages
        # should depend on specific sub-packages, not this meta-package.
        copy(self, "*.hpp",
             src=os.path.join(self.source_folder, "core", "api"),
             dst=os.path.join(self.package_folder, "include"),
             keep_path=True)
        copy(self, "*.hpp",
             src=os.path.join(self.source_folder, "consumer", "api"),
             dst=os.path.join(self.package_folder, "include"),
             keep_path=True)
        copy(self, "*.hpp",
             src=os.path.join(self.source_folder, "producer", "api"),
             dst=os.path.join(self.package_folder, "include"),
             keep_path=True)

    def package_info(self):
        """This is a header-only meta-package; all symbols come from sub-packages."""
        # Inform consumers to use specific sub-packages
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
        # Include paths are set by sub-packages, not this meta-package
        self.cpp_info.includedirs = []
