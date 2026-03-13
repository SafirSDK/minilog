{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  name = "minilog-dev";

  packages = with pkgs; [
    # Build system
    cmake
    ninja
    pkg-config

    # Compilers
    gcc
    clang

    # Boost (used directly on Linux; Conan only manages it on Windows)
    boost

    # Dependency management (for Windows cross-build and conanfile.py)
    #conan

    # Useful dev tools
    gdb
    gh
  ];

  shellHook = ''
    echo "minilog dev shell — $(cmake --version | head -1), $(gcc --version | head -1)"
    echo "Boost: $(pkg-config --modversion boost 2>/dev/null || echo 'available via nix')"
  '';
}
