# shellcheck shell=bash
# Sources of the macOS host toolchain (build-mac-host.sh). Sourced by bash; every value is checked after download.
#
# They are the sources of the pinned VitaSDK release, sdk-snapshot-20260926.790.1 (https://github.com/vitasdk/autobuilds):
# its release.json names vitasdk/buildscripts revision 3c0b9c7b..., and cmake/Components.cmake, cmake/recipes/*.cmake and patches/
# of that revision say which versions and which configure options the SDK's host programs were built with. Nothing is copied
# from there into this repository: the patches (GPL, like the programs they patch) are downloaded when the toolchain is built,
# from the revision below, and checked against these hashes.
#
# Changing anything in this folder gives the toolchain another cache key (kits.yml), so it is rebuilt once.

BUILDSCRIPTS_REV=3c0b9c7b2c8ecb7bf68a674451a295f724b7fafb
BUILDSCRIPTS_RAW=https://raw.githubusercontent.com/vitasdk/buildscripts/$BUILDSCRIPTS_REV

GCC_VERSION=15.2.0;     GCC_SHA256=438fd996826b0c82485a29da03a72d71d6e3541a83ec702df4271f6fe025d24e
BINUTILS_VERSION=2.46.1; BINUTILS_SHA256=e127a709cba24c76de8936cb7083dd768f28cd37eb010492e2f19b71eb1294e4
GMP_VERSION=6.3.0;      GMP_SHA256=ac28211a7cfb609bae2e2c8d6058d66c8fe96434f740cf6fe2e47b000d1c20cb
MPFR_VERSION=4.2.2;     MPFR_SHA256=9ad62c7dc910303cd384ff8f1f4767a655124980bb6d8650fe62c815a231bb7b
MPC_VERSION=1.3.1;      MPC_SHA256=ab642492f5cf882b74aa0cb730cd410a81edcdbec895183ce930e706c1c759b8
# The SDK builds ISL from the git tag isl-0.24; this is the release tarball of the same version, which GCC's own
# contrib/download_prerequisites uses (the SDK's ISL_HASH in Components.cmake is not checked by its recipe, which uses git).
ISL_VERSION=0.24;       ISL_SHA256=fcf78dd9656c10eb8cf9fbd5f59a0b6b01386205fe1934b3b287a0a1898145c0
ZLIB_VERSION=1.3.1;     ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
LIBELF_VERSION=0.8.13;  LIBELF_SHA256=591a9b4ec81c1f2042a97aa60564e0cb79d041c52faa7416acb38bc95bd2c76d
LIBYAML_VERSION=0.2.2;  LIBYAML_SHA256=4a9100ab61047fd9bd395bcef3ce5403365cafd55c1e0d0299cde14958e47be9
LIBZIP_VERSION=1.11.4;  LIBZIP_SHA256=8a247f57d1e3e6f6d11413b12a6f28a9d388de110adc0ec608d893180ed7097b
VITA_TOOLCHAIN_COMMIT=e8bc8f70ee5d3036f1562486cbc6065b52a8d173

# patches/ of the buildscripts revision above: PATH SHA-256
PATCHES='
patches/binutils/0001-vita.patch 974d810a72a9060c8244a71ddc5e7e08065afb7fb877b2294f4a5978de84396e
patches/binutils/0002-fix-broken-reloc.patch 7ff83fe426a49c0cf79ea2f290a870988ed2f507345d3d6ef6c5d0ceb9ddbd65
patches/binutils/0003-fix-elf-vaddr.patch eeebce9e151484642eccfb595ce323e011b60f44f27a5df657158b3b1e0f9c9d
patches/binutils/0004-fix-interworking-veneers.patch e294703aa8fe8108e06a8b8f6dd15e1c1c2d1ca559c36489f91c305f2f00ec63
patches/binutils/0005-genscripts-mkdir.patch e1ba410f28b48bb2df1ca2c6946f81e3f68a5b31acd7c529d4403d79781aa7ae
patches/gcc/0001-vita-target.patch 2017f868524686ac55b096066a2f8459d5fd972e75ca721443bc2f60f4c6b8d8
patches/gcc/0002-vita-driver.patch 73d192fb92f26947f0cc93cfd613294dd35a30e50dedf0c62b3a060ebf11af46
patches/gcc/0003-libgomp-vita.patch 90dd62616b34b1f13cbc2ca812aa6794e7119f6cb0a2973768c4a7ebc9f2a5fe
patches/gcc/0004-host-compat.patch 4a2face839965875f0fc7d4936f11f39b8b53b35d1abe7b764ca4e40a1228ed9
patches/libelf-configure-in.patch 4f6fcfff85046847ed1e93e0b701103b4b1cab97f4e76400c553154c0fb4f9b5
config.guess 50205cf3ec5c7615b17f937a0a57babf4ec5cd0aade3d7b3cccbe5f1bf91a7ef
config.sub 26b852f75a637448360a956931439f7e818bf63150eaadb9b85484347628d1fd
'
