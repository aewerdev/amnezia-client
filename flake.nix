{
  description = "AmneziaVPN native Linux build environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/fd1462031fdee08f65fd0b4c6b64e22239a77870";

  outputs = { nixpkgs, ... }:
    let
      supportedSystems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          qtPackages = with pkgs.qt6; [
            qtbase
            qtdeclarative
            qtimageformats
            qt5compat
            qtremoteobjects
            qtshadertools
            qtsvg
            qttools
            qtwayland
          ];
          qtCmakePrefixPath = pkgs.lib.makeSearchPath "" (map pkgs.lib.getDev qtPackages);
          qtToolPath = pkgs.lib.makeSearchPath "bin" (map pkgs.lib.getDev [
            pkgs.qt6.qtremoteobjects
            pkgs.qt6.qttools
          ]);
        in
        {
          default = pkgs.mkShell {
            name = "amnezia-vpn";

            packages = with pkgs; [
              bash
              cacert
              cmake
              conan
              curl
              gcc
              git
              libsecret
              ninja
              patchelf
              pkg-config
              python3
              xz
              zstd
              qt6.wrapQtAppsHook
            ] ++ qtPackages;

            shellHook = ''
              export CMAKE_PREFIX_PATH="${qtCmakePrefixPath}''${CMAKE_PREFIX_PATH:+:}''${CMAKE_PREFIX_PATH:-}"
              export PATH="${qtToolPath}:$PATH"
            '';
          };
        });
    };
}
