from conan import ConanFile


class MinilogConan(ConanFile):
    name = "minilog"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        # On Linux, Boost is provided by the distro (e.g. libboost-all-dev).
        # Conan only manages Boost on Windows.
        if self.settings.os == "Windows":
            self.requires("boost/1.87.0")

    def configure(self):
        if self.settings.os == "Windows":
            self.options["boost/*"].shared = False
