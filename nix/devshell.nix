# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Development shell with everything the Meson builds, the unit tests, the
# helper scripts and the CI checks need. The pinned wrap subprojects are
# exposed through MESON_PACKAGE_CACHE_DIR, so `meson setup` works offline.
{
  lib,
  stdenv,
  mkShell,
  meson,
  ninja,
  pkg-config,
  cmake,
  git,
  sdl2-compat,
  sdl3,
  codec2,
  catch2_3,
  readline,
  libusb1,
  dfu-util,
  openocd,
  gcovr,
  clang-tools,
  reuse,
  nixfmt,
  statix,
  deadnix,
  ffmpeg,
  python3,
  miosix-toolchain,
  radio_tool,
  subprojects,
}:

let
  # The exact sdl3 that sdl2-compat links against and dlopens at runtime:
  # AddressSanitizer intercepts dlopen and loses sdl2-compat's RUNPATH, so
  # `meson test` on a -Dasan=true build aborts with "Failed loading SDL3
  # library." unless the loader can find it (same workaround as
  # nix/emulator.nix).
  sdl3ForCompat = lib.getLib (sdl3.override { traySupport = false; });
in
mkShell {
  name = "openrtx";

  packages = [
    meson
    ninja
    pkg-config
    cmake
    git
    sdl2-compat
    codec2
    catch2_3
    readline
    libusb1
    dfu-util
    openocd
    gcovr
    clang-tools
    reuse
    nixfmt
    statix
    deadnix
    ffmpeg
    radio_tool
    # requirements.txt plus what scripts/*.py import
    (python3.withPackages (
      ps: with ps; [
        setuptools
        pyusb
        intelhex
        urllib3
        numpy
        cairosvg
        pillow
      ]
    ))
  ]
  ++ lib.optional (lib.meta.availableOn stdenv.hostPlatform miosix-toolchain) miosix-toolchain;

  MESON_PACKAGE_CACHE_DIR = subprojects.packageCache;

  shellHook = ''
    export LD_LIBRARY_PATH=${sdl3ForCompat}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
    echo "OpenRTX development shell"
    echo "  meson setup build_linux && meson compile -C build_linux openrtx_linux"
    echo "  meson setup --cross-file cross_cm4.txt build_cm4 && meson compile -C build_cm4 openrtx_md3x0_wrap"
    echo "  (wrap subprojects are served offline from MESON_PACKAGE_CACHE_DIR)"
    echo "  meson setup -Dasan=true -Dubsan=true build_san && meson test -C build_san"
  '';
}
