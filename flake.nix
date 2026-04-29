{
  description = "tenet chat server and tenet-bot development shell";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { nixpkgs, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs {
            inherit system;
            config.problems.handlers.sqlite-vss.broken = "ignore";
          };
          sqliteVss = pkgs.sqlite-vss.overrideAttrs (old: {
            postPatch = (old.postPatch or "") + ''
              grep -q '#include <stdexcept>' src/sqlite-vss.cpp || sed -i '1i #include <stdexcept>' src/sqlite-vss.cpp
            '';
          });
        in {
          default = pkgs.mkShell {
            packages = [
              pkgs.clang
              pkgs.gnumake
              pkgs.ollama
              pkgs.pkg-config
              pkgs.sqlite
              pkgs.sqlite.dev
              sqliteVss
            ];

            shellHook = ''
              export LD_LIBRARY_PATH="${pkgs.lib.makeLibraryPath [ pkgs.sqlite sqliteVss ]}:$LD_LIBRARY_PATH"
              if [ -z "$TENET_BOT_VECTOR_EXTENSION" ]; then
                TENET_BOT_VECTOR_EXTENSION="$(find ${sqliteVss}/lib -name 'vector0*.so' -print -quit 2>/dev/null || true)"
                export TENET_BOT_VECTOR_EXTENSION
              fi
              if [ -z "$TENET_BOT_VSS_EXTENSION" ]; then
                TENET_BOT_VSS_EXTENSION="$(find ${sqliteVss}/lib -name 'vss0*.so' -print -quit 2>/dev/null || true)"
                export TENET_BOT_VSS_EXTENSION
              fi
              echo "tenet dev shell: run 'make', then './tenet-bot --help'"
            '';
          };
        });
    };
}
