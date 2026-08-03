{
  description = "DuckDB with the cache_httpfs (duck-read-cache-fs) extension statically linked";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-25.11";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        # The duckdb/ and duckdb-httpfs/ submodules must be present in the
        # flake source. They are only included when the flake is fetched with
        # `?submodules=1`, e.g.  nix build '.?submodules=1'
        submodulesPresent = builtins.pathExists (self + "/duckdb/CMakeLists.txt");

        # Build against [tgtPkgs] (native, or a pkgsCross set for cross builds).
        # explicitPlatform bypasses DuckDB's build step that compiles a tiny
        # platform-detection binary FOR THE TARGET and then RUNS it -- which
        # aborts with "Exec format error" when cross-compiling. Passing the
        # platform string skips that binary entirely (CMakeLists.txt guards it
        # with `if(NOT DUCKDB_EXPLICIT_PLATFORM)`).
        mkDuckdb = tgtPkgs: explicitPlatform: tgtPkgs.stdenv.mkDerivation {
          pname = "duckdb-cache-httpfs";
          # DuckDB core version pinned by the duckdb/ submodule.
          version = "1.5.4";

          src =
            if submodulesPresent then self
            else throw ''
              The duckdb/ submodule is missing from the flake source.
              Build with submodules enabled, e.g.:
                nix build '.?submodules=1'
                nix build 'git+https://github.com/Meandres/duck-read-cache-fs?submodules=1'
            '';

          # buildPackages = build-host tools (== tgtPkgs natively); plain
          # attrs = target libraries. The split is what makes cross work.
          nativeBuildInputs = with tgtPkgs.buildPackages; [ cmake ninja python3 ];
          buildInputs = with tgtPkgs; [ openssl curl ];

          # Mirrors `make release` from extension-ci-tools/makefiles/duckdb_extension.Makefile:
          # cmake is pointed at the vendored DuckDB tree, which pulls in this
          # extension (and its bundled duckdb-httpfs sources) via
          # DUCKDB_EXTENSION_CONFIGS -> extension_config.cmake.
          cmakeDir = "../duckdb";
          cmakeBuildType = "Release";
          cmakeFlags = [
            "-DEXTENSION_STATIC_BUILD=1"
            "-DENABLE_EXTENSION_AUTOLOADING=1"
            "-DENABLE_EXTENSION_AUTOINSTALL=0"
            "-DENABLE_UNITTEST_CPP_TESTS=FALSE"
            # Store copies have no .git; give DuckDB its version explicitly.
            "-DOVERRIDE_GIT_DESCRIBE=v1.5.4"
          ] ++ pkgs.lib.optional (explicitPlatform != null)
            "-DDUCKDB_EXPLICIT_PLATFORM=${explicitPlatform}";
          preConfigure = ''
            cmakeFlagsArray+=(
              "-DDUCKDB_EXTENSION_CONFIGS=$PWD/extension_config.cmake"
              "-DUNITTEST_ROOT_DIRECTORY=$PWD"
            )
          '';

          enableParallelBuilding = true;

          # DuckDB's `install` target ships libduckdb + headers + the shell.
          # Additionally install the loadable extension for use with stock
          # duckdb binaries (LOAD '<path>/cache_httpfs.duckdb_extension').
          postInstall = ''
            mkdir -p $out/lib/duckdb-extensions
            cp extension/cache_httpfs/cache_httpfs.duckdb_extension \
              $out/lib/duckdb-extensions/
          '';

          meta = with pkgs.lib; {
            description = "DuckDB shell and library with the cache_httpfs remote-read caching extension built in";
            homepage = "https://github.com/dentiny/duck-read-cache-fs";
            license = licenses.mit;
            platforms = platforms.linux ++ platforms.darwin;
            mainProgram = "duckdb";
          };
        };

        # Native build for this system.
        duckdb-cache-httpfs = mkDuckdb pkgs null;
        # aarch64 cross build (for Graviton). Cross-compiled from an x86 host;
        # exposed under the builder's system set, so on x86_64-linux this is
        # `packages.x86_64-linux.duckdb-cache-httpfs-aarch64`.
        duckdb-cache-httpfs-aarch64 =
          mkDuckdb pkgs.pkgsCross.aarch64-multiplatform "linux_arm64";
      in
      {
        packages = {
          default = duckdb-cache-httpfs;
          inherit duckdb-cache-httpfs duckdb-cache-httpfs-aarch64;
        };

        apps.default = {
          type = "app";
          program = "${duckdb-cache-httpfs}/bin/duckdb";
        };

        devShells.default = pkgs.mkShell {
          name = "duck-read-cache-fs-devshell";
          inputsFrom = [ duckdb-cache-httpfs ];
          packages = with pkgs; [ ccache gdb clang-tools ];
          # `make release GEN=ninja` works inside this shell.
          GEN = "ninja";
        };
      });
}
