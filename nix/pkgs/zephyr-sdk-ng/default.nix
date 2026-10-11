{ stdenv, lib, fetchurl, autoPatchelfHook ? null, makeBinaryWrapper, writeTextFile
, xz, python3, which, hidapi, libftdi1, libusb1, cmake, wget, file
, version ? "1.0.1"
, toolchains ? "all"
, withLlvm ? false
, ncurses ? null
, python312 ? null
, libxcrypt ? null
}:

let
  pname = "zephyr-sdk";
  system = lib.splitString "-" stdenv.hostPlatform.system;
  host = "${lib.elemAt system 1}-${lib.elemAt system 0}";
  base-url = "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${version}/";

  hashes = (import ./zephyr-sdk-hash.nix { inherit lib; }).${version}.${host};

  gnu = hashes.gnu or {};
  llvm = hashes.llvm or {};
  gnu-toolchains = gnu.toolchains or hashes.toolchains or {};

  install-toolchains =
    if toolchains == "all"
    then builtins.attrNames gnu-toolchains
    else toolchains;

  fetch = attrs: fetchurl { url = "${base-url}${attrs.file}"; hash = attrs.hash; };

in stdenv.mkDerivation {
  inherit pname version;

  srcs = [ (fetch hashes.minimal) ]
    ++ map (tc: fetch gnu-toolchains.${tc}) install-toolchains
    ++ lib.optionals (withLlvm && llvm ? toolchain) [ (fetch llvm.toolchain) ];

  sourceRoot = "${pname}-${version}";

  nativeBuildInputs = [ makeBinaryWrapper which file cmake wget ]
    ++ lib.optionals stdenv.isLinux [ stdenv.cc.bintools autoPatchelfHook ];

  buildInputs = [ xz python3 hidapi libftdi1 libusb1 ]
    ++ lib.optionals stdenv.isLinux [ ncurses python312 libxcrypt ];

  autoPatchelfIgnoreMissingDeps = [ "libcrypt.so.1" ];

  configurePhase = "true";
  buildPhase = "true";

  installPhase = ''
    mkdir -p $out/zephyr-sdk
    cp -r ./* $out/zephyr-sdk/

    mkdir -p $out/zephyr-sdk/gnu
    for tc in ${lib.concatStringsSep " " install-toolchains}; do
      if [ -d "../$tc" ]; then
        cp -r "../$tc" "$out/zephyr-sdk/gnu/$tc"
      fi
    done

    ${lib.optionalString withLlvm ''
    if [ -d "../llvm" ]; then
      cp -r ../llvm $out/zephyr-sdk/llvm
    fi
    ''}

    patchShebangs $out/zephyr-sdk
    cd $out/zephyr-sdk
    ./setup.sh -h
  '';

  postFixup = ''
    find $out -type f -perm -a+x -name '*-py' | while read prog; do
      wrapProgram "$prog" \
        --set NIX_PYTHONPREFIX ${python3} \
        --set NIX_PYTHONEXECUTABLE ${python3}/bin/python3 \
        --set NIX_PYTHONPATH ${python3}/lib/python3/site-packages
    done
  '';

  setupHook = writeTextFile {
    name = "zephyr-sdk-setup-hook";
    text = ''
      export ZEPHYR_SDK_INSTALL_DIR="@out@/zephyr-sdk"
      export ZEPHYR_TOOLCHAIN_VARIANT="${if withLlvm then "zephyr/llvm" else "zephyr/gnu"}"
    '';
  };
}
