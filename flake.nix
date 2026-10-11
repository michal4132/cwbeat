{
  description = "CWbeat Zephyr module development environment";

  inputs.nixpkgs.url = "nixpkgs/release-26.05";

  outputs = { nixpkgs, ... }:
    let
      supportedSystems = [ "aarch64-darwin" "aarch64-linux" "x86_64-linux" ];
    in
    {
      devShells = nixpkgs.lib.genAttrs supportedSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          python = pkgs.python313;
          zephyr-sdk = pkgs.callPackage ./nix/pkgs/zephyr-sdk-ng {
            toolchains = [ "arm-zephyr-eabi" ];
          };
        in
        {
          default = pkgs.mkShell {
            name = "cwbeat-zephyr-module-dev";
            shellName = "cwbeat-zephyr-module-dev";
            hardeningDisable = [ "fortify" ];
            packages = with pkgs; [
              bash-completion
              ccache
              cmake
              clang-tools
              coreutils
              dtc
              git
              gnumake
              ncurses
              ninja
              probe-rs-tools
              python
              python313Packages.west
              uv
              zephyr-sdk
            ];
            shellHook = ''
              if command -v tput >/dev/null 2>&1 && \
                 [ "$(tput colors 2>/dev/null || echo 0)" -ge 8 ]; then
                export PS1="\[\e[1;34m\][$shellName]\[\e[0m\] $PS1"
              else
                export PS1="[$shellName] $PS1"
              fi

              export VIRTUAL_ENV_DISABLE_PROMPT=1
              if [ ! -d "../.venv" ]; then
                uv venv --python ${python}/bin/python3 ../.venv
              fi
              source ../.venv/bin/activate

              source "${pkgs.bash-completion}/share/bash-completion/bash_completion"
            '';
          };
        });
    };
}
