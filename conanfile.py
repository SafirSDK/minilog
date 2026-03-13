from conan import ConanFile


class MinilogConan(ConanFile):
    name = "minilog"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    # Compiled Boost libraries we actually use
    _boost_keep = {
        "system",       # required by Asio
        "json",         # JSONL output
        "test",         # unit tests (unit_test_framework)
        "container",    # required by json
    }

    # All compiled Boost libraries that have a without_* option in the recipe
    _boost_all = {
        "atomic", "charconv", "chrono", "cobalt", "container", "context",
        "contract", "coroutine", "date_time", "exception", "fiber", "filesystem",
        "graph", "graph_parallel", "iostreams", "json", "locale", "log", "math",
        "mpi", "nowide", "process", "program_options", "python", "random", "regex",
        "serialization", "stacktrace", "system", "test", "thread", "timer",
        "type_erasure", "url", "wave",
    }

    def requirements(self):
        # On Linux, Boost is provided by the distro (e.g. libboost-all-dev).
        # Conan only manages Boost on Windows.
        if self.settings.os == "Windows":
            self.requires("boost/1.87.0")

    def configure(self):
        if self.settings.os == "Windows":
            self.options["boost/*"].shared = False
            for lib in self._boost_all - self._boost_keep:
                setattr(self.options["boost/*"], f"without_{lib}", True)
