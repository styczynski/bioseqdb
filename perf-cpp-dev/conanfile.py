from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy
import os


class PerfCppConan(ConanFile):
    name = "perf-cpp"
    version = "0.13.0"
    license = "LGPL-3.0-only"
    author = "Jan Muehlig"
    url = "https://github.com/jmuehlig/perf-cpp"
    homepage = "https://github.com/jmuehlig/perf-cpp"
    description = (
        "Lightweight C++17 library for hardware performance counter "
        "monitoring and sampling using the Linux perf subsystem."
    )
    topics = (
        "performance",
        "profiling",
        "perf",
        "hardware-counters",
        "sampling",
        "pebs",
        "ibs",
        "linux",
    )

    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
    }

    exports_sources = "CMakeLists.txt", "src/*", "include/*", "LICENSE", ".clang-tidy"

    def validate(self):
        if self.settings.os != "Linux":
            raise ConanInvalidConfiguration(
                "perf-cpp requires Linux (uses perf_event_open syscall)."
            )

    def config_options(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["BUILD_LIB_SHARED"] = self.options.shared
        tc.variables["BUILD_EXAMPLES"] = False
        tc.variables["BUILD_TESTS"] = False
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()
        ## CMakeLists.txt uses a custom CMAKE_INSTALL_LIBRARY_DIR (CACHE PATH), which CMake
        ## resolves to an absolute path in the build tree. Copy the library explicitly.
        copy(self, "*.a", src=self.build_folder, dst=os.path.join(self.package_folder, "lib"), keep_path=False)
        copy(self, "*.so*", src=self.build_folder, dst=os.path.join(self.package_folder, "lib"), keep_path=False)

    def package_info(self):
        self.cpp_info.libs = ["perf-cpp"]
        self.cpp_info.set_property("cmake_file_name", "perf-cpp")
        self.cpp_info.set_property("cmake_target_name", "perf-cpp::perf-cpp")
