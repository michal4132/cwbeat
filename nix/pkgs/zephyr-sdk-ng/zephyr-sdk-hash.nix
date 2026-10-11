{ lib }:
{
  "1.0.1" = {
    "darwin-aarch64" = {
      minimal = {
        file = "zephyr-sdk-1.0.1_macos-aarch64_minimal.tar.xz";
        hash = "sha256-hnBjkB85UophdagOvCA2e9bLRAWT5+JlDtowOS8fa2U=";
      };
      gnu.toolchains."arm-zephyr-eabi" = {
        file = "toolchain_gnu_macos-aarch64_arm-zephyr-eabi.tar.xz";
        hash = "sha256-QAjttdSEDNmUrt1/Ewm/tj5yQ3KdV4OevxzIPB8XyIY=";
      };
    };
    "linux-aarch64" = {
      minimal = {
        file = "zephyr-sdk-1.0.1_linux-aarch64_minimal.tar.xz";
        hash = "sha256-15xb/GjmeUiGWb6iiaQCblKmTwMziHXIychQ//E87jA=";
      };
      gnu.toolchains."arm-zephyr-eabi" = {
        file = "toolchain_gnu_linux-aarch64_arm-zephyr-eabi.tar.xz";
        hash = "sha256-uYBbaR8vCokmySaUyuN40Fuge3arynReIW/MUnU8xNY=";
      };
    };
    "linux-x86_64" = {
      minimal = {
        file = "zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz";
        hash = "sha256-ypvA/2b6/KHaydWSo22VPPFtCWqdCbHANX8CHPn2p+s=";
      };
      gnu.toolchains."arm-zephyr-eabi" = {
        file = "toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz";
        hash = "sha256-IbhZgctaGBjZvFPYKvgPIIlG7AOLmC/xkHKHVy7TpjQ=";
      };
    };
  };
}
