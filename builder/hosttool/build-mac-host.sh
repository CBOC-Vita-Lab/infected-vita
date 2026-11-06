#!/usr/bin/env bash
# Builds the HOST programs of the Infected builder kit for macOS (the ones the kit takes from VitaSDK's prebuilt autobuilds on the
# other systems) with a low MACOSX_DEPLOYMENT_TARGET, so that the kit runs on old Macs. Run it on a Mac of the kind the programs are
# for (Intel for amd64, Apple silicon for arm64): it builds natively.
#
#   build-mac-host.sh --arch amd64|arm64 --target 10.13 [--phase deps|gcc|all] [--root DIR] [--tmp DIR]
#
#   --phase deps  zlib, libzip, libelf, libyaml, gmp, mpfr, mpc, isl, binutils, vita-toolchain  ->  ROOT/deps and ROOT/vitasdk
#   --phase gcc   GCC (needs the result of "deps"), then the files of ../kit/lists/host-files.txt  ->  ROOT/out   (the "host SDK"
#                 make-kit.sh host --host-sdk takes: the same folder layout as the VitaSDK, only with the listed files)
#   ROOT          default /opt/vita-host. It is the prefix GCC and binutils are configured with (they find their parts relative to
#                 their own place afterwards, as in the VitaSDK); a fixed place keeps a cached "deps" phase valid.
#   TMP           sources, build folders and logs (default: a new folder in $TMPDIR). Logs: TMP/logs.
#
# What is built, with which sources and options: pins.sh and the comments below. The recipe is the one of vitasdk/buildscripts at the
# revision named in pins.sh (cmake/Components.cmake, cmake/recipes/BuildHostDependencies.cmake, GccCommonArgs.cmake, BuildGccFinal.cmake),
# the options of every component unchanged. What differs from the SDK builds, and why:
#   * MACOSX_DEPLOYMENT_TARGET / -mmacosx-version-min: the point of this script.
#   * -O2 for every component (the SDK builds GCC with only "-fbracket-depth=512", i.e. without optimisation, and vita-toolchain too).
#     Optimisation of the compiler does not change what the compiler writes.
#   * -Wl,-x -Wl,-S -Wl,-no_fixup_chains at link time: no local symbols or debug map in the programs (a stripped Mach-O would lose
#     its signature, which Apple silicon requires, so the linker is asked not to put them in), and the classic dyld fixups that
#     macOS before 12 can read (the linker of recent Xcode versions would use chained fixups from a target of 12 on only; the flag makes
#     sure of it).
#   * GMP is configured with an explicit --host: its own CPU detection would pick the instructions of the CI machine (it picks
#     haswell/skylake code on an Intel runner), and the programs have to run on every Intel Mac that has macOS 10.13.
#   * Only the programs the kit ships are built and kept: gcc all-gcc + lto plugin, binutils, vita-elf-create, vita-make-fself.
#   * Patches are applied with git apply (stricter than fuzz 0), GMP/MPFR/MPC/ISL/zlib as release tarballs (ISL: see pins.sh).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=pins.sh
. "$HERE/pins.sh"

ARCH=""; TARGET=""; PHASE=all; ROOT=/opt/vita-host; TMP=""
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH=$2; shift 2;; --target) TARGET=$2; shift 2;; --phase) PHASE=$2; shift 2;;
    --root) ROOT=$2; shift 2;; --tmp) TMP=$2; shift 2;;
    *) echo "unknown argument $1" >&2; exit 2;;
  esac
done
case "$ARCH" in
  amd64) TRIPLE=x86_64-apple-darwin; OSX_ARCH=x86_64; [ "$(uname -m)" = x86_64 ] || { echo "amd64 needs an Intel Mac (this is $(uname -m))" >&2; exit 2; };;
  arm64) TRIPLE=aarch64-apple-darwin; OSX_ARCH=arm64; [ "$(uname -m)" = arm64 ] || { echo "arm64 needs an Apple silicon Mac (this is $(uname -m))" >&2; exit 2; };;
  *) echo "--arch amd64|arm64" >&2; exit 2;;
esac
[ -n "$TARGET" ] || { echo "--target (the lowest macOS version, e.g. 10.13) is required" >&2; exit 2; }
case "$PHASE" in deps|gcc|all) ;; *) echo "--phase deps|gcc|all" >&2; exit 2;; esac
[ "$(uname -s)" = Darwin ] || { echo "this builds for macOS and runs on macOS" >&2; exit 2; }

TARGET_TRIPLE=arm-vita-eabi
PKGVERSION="GNU Tools for ARM Embedded Processors"       # buildscripts CMakeLists.txt: pkgversion
mkdir -p "$ROOT" 2>/dev/null || true
[ -w "$ROOT" ] || { echo "$ROOT is not writable: sudo mkdir -p $ROOT && sudo chown \$USER $ROOT" >&2; exit 1; }
ROOT=$(cd "$ROOT" && pwd)
DEPS=$ROOT/deps
PREFIX=$ROOT/vitasdk
OUT=$ROOT/out
[ -n "$TMP" ] || TMP=$(mktemp -d "${TMPDIR:-/tmp}/hosttool.XXXXXX")
mkdir -p "$TMP"; TMP=$(cd "$TMP" && pwd)
DL=$TMP/dl; SRC=$TMP/src; BLD=$TMP/build; LOGS=$TMP/logs
mkdir -p "$DL" "$SRC" "$BLD" "$LOGS" "$DEPS" "$PREFIX"
NCPU=$(sysctl -n hw.ncpu)

# ------------------------------------------------------------------------------------------------------------------ environment
export MACOSX_DEPLOYMENT_TARGET=$TARGET
export CC=cc CXX=c++
LDX="-Wl,-x -Wl,-S"
echo 'int main(void) { return 0; }' > "$TMP/probe.c"
if cc -mmacosx-version-min="$TARGET" "$TMP/probe.c" -o "$TMP/probe" -Wl,-no_fixup_chains 2>/dev/null; then
  LDX="$LDX -Wl,-no_fixup_chains"
else
  echo "note: this linker does not know -no_fixup_chains"
fi
OPT="-O2 -mmacosx-version-min=$TARGET"
export CFLAGS="$OPT" CXXFLAGS="$OPT" LDFLAGS="-mmacosx-version-min=$TARGET $LDX"
CMAKE_COMMON=(-DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_DEPLOYMENT_TARGET=$TARGET" "-DCMAKE_OSX_ARCHITECTURES=$OSX_ARCH"
              "-DCMAKE_C_FLAGS=$OPT" "-DCMAKE_EXE_LINKER_FLAGS=$LDFLAGS" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF)
echo "host $(uname -m), macOS $(sw_vers -productVersion), $(cc --version | head -1), cmake $(cmake --version | head -1 | awk '{print $3}')"
echo "target macOS $TARGET, triple $TRIPLE, $NCPU CPUs; root $ROOT; tmp $TMP"

# ------------------------------------------------------------------------------------------------------------------ helpers
TIMINGS=$TMP/timings.txt
step() {                      # step NAME FUNCTION...: runs it in a subshell with its output in LOGS/NAME.log; a failure ends the script
  local name=$1; shift
  local t0=$SECONDS
  echo "== $name"
  ( "$@" ) >"$LOGS/$name.log" 2>&1 &
  local pid=$!
  ( while sleep 240; do kill -0 "$pid" 2>/dev/null || exit 0; echo "   ... $name is still running ($((SECONDS - t0)) s)"; done ) &
  local hb=$!
  local rc=0
  wait "$pid" || rc=$?
  kill "$hb" 2>/dev/null || true
  wait "$hb" 2>/dev/null || true
  printf '%-22s %5d s\n' "$name" $((SECONDS - t0)) >> "$TIMINGS"
  if [ "$rc" != 0 ]; then
    echo "---- last lines of $LOGS/$name.log"; tail -n 150 "$LOGS/$name.log"
    echo "FAILED: $name (exit $rc)"; exit 1
  fi
  echo "   $name: $((SECONDS - t0)) s"
}

sha_ok() { echo "$2  $1" | shasum -a 256 -c - >/dev/null 2>&1; }

fetch() {                     # fetch FILE SHA256 URL...   (into DL; the first URL that gives the right bytes wins)
  local f=$DL/$1 sha=$2; shift 2
  if [ -f "$f" ] && sha_ok "$f" "$sha"; then return 0; fi
  local u
  for u in "$@"; do
    if curl -fsSL --retry 4 --retry-delay 5 --retry-all-errors -o "$f.part" "$u" && sha_ok "$f.part" "$sha"; then mv "$f.part" "$f"; return 0; fi
    echo "   not usable: $u"
  done
  echo "could not download $1 with SHA-256 $sha" >&2; return 1
}
gnu() { local d=$1 f=$2; echo "https://mirrors.kernel.org/gnu/$d/$f https://ftpmirror.gnu.org/gnu/$d/$f https://ftp.gnu.org/gnu/$d/$f"; }

fetch_patches() {
  local p sha
  while read -r p sha; do
    [ -n "$p" ] || continue
    mkdir -p "$DL/buildscripts/$(dirname "$p")"
    fetch "buildscripts/$p" "$sha" "$BUILDSCRIPTS_RAW/$p"
  done <<< "$PATCHES"
}
bs() { echo "$DL/buildscripts/$1"; }              # a file of buildscripts

unpack() {                    # unpack ARCHIVE → SRC/<top folder>
  tar -xf "$DL/$1" -C "$SRC"
}
apply() {                     # apply DIR PATCH...  (a patch series, -p1; git apply allows no fuzz)
  local dir=$1; shift
  local p
  for p in "$@"; do echo "applying $(basename "$p")"; (cd "$dir" && git apply --whitespace=nowarn -p1 "$p"); done
}
need_tool() { command -v "$1" >/dev/null 2>&1 || return 1; }

# ------------------------------------------------------------------------------------------------------------------ phase deps
do_fetch_all() {
  fetch_patches
  # shellcheck disable=SC2046
  fetch "binutils-$BINUTILS_VERSION.tar.xz" "$BINUTILS_SHA256" $(gnu binutils "binutils-$BINUTILS_VERSION.tar.xz")
  # shellcheck disable=SC2046
  fetch "gmp-$GMP_VERSION.tar.bz2" "$GMP_SHA256" $(gnu gmp "gmp-$GMP_VERSION.tar.bz2")
  # shellcheck disable=SC2046
  fetch "mpfr-$MPFR_VERSION.tar.bz2" "$MPFR_SHA256" $(gnu mpfr "mpfr-$MPFR_VERSION.tar.bz2")
  # shellcheck disable=SC2046
  fetch "mpc-$MPC_VERSION.tar.gz" "$MPC_SHA256" $(gnu mpc "mpc-$MPC_VERSION.tar.gz")
  fetch "isl-$ISL_VERSION.tar.bz2" "$ISL_SHA256" "https://gcc.gnu.org/pub/gcc/infrastructure/isl-$ISL_VERSION.tar.bz2" \
        "https://libisl.sourceforge.io/isl-$ISL_VERSION.tar.bz2"
  fetch "zlib-$ZLIB_VERSION.tar.gz" "$ZLIB_SHA256" "https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz"
  fetch "libelf-$LIBELF_VERSION.tar.gz" "$LIBELF_SHA256" "https://github.com/vitasdk/artifacts/releases/download/libelf-$LIBELF_VERSION/libelf-$LIBELF_VERSION.tar.gz"
  fetch "yaml-$LIBYAML_VERSION.tar.gz" "$LIBYAML_SHA256" "https://pyyaml.org/download/libyaml/yaml-$LIBYAML_VERSION.tar.gz"
  fetch "libzip-$LIBZIP_VERSION.tar.xz" "$LIBZIP_SHA256" "https://libzip.org/download/libzip-$LIBZIP_VERSION.tar.xz"
  local d=$SRC/vita-toolchain
  if [ ! -d "$d/.git" ]; then
    git init -q "$d"
    git -C "$d" remote add origin https://github.com/vitasdk/vita-toolchain
  fi
  git -C "$d" fetch -q --depth 1 origin "$VITA_TOOLCHAIN_COMMIT"
  git -C "$d" checkout -q --force FETCH_HEAD
  [ "$(git -C "$d" rev-parse HEAD)" = "$VITA_TOOLCHAIN_COMMIT" ]
  git -C "$d" submodule update --init --recursive --depth 1
  git -C "$d" submodule status
}

b_zlib() {
  unpack "zlib-$ZLIB_VERSION.tar.gz"
  cd "$SRC/zlib-$ZLIB_VERSION"
  ./configure --static --prefix="$DEPS"
  make -j"$NCPU"
  make install
}

b_libzip() {                  # libzip, as the recipe: only the library, no optional crypto/compression/tools
  unpack "libzip-$LIBZIP_VERSION.tar.xz"
  cmake -S "$SRC/libzip-$LIBZIP_VERSION" -B "$BLD/libzip" "${CMAKE_COMMON[@]}" -DCMAKE_INSTALL_PREFIX="$DEPS" -DCMAKE_INSTALL_LIBDIR=lib \
    -DZLIB_INCLUDE_DIR="$DEPS/include" -DZLIB_LIBRARY="$DEPS/lib/libz.a" \
    -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF -DENABLE_WINDOWS_CRYPTO=OFF \
    -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF
  cmake --build "$BLD/libzip" -j "$NCPU"
  cmake --install "$BLD/libzip"
}

b_libelf() {                  # patched configure.in → autoreconf (the recipe does the same), then plain configure/make
  unpack "libelf-$LIBELF_VERSION.tar.gz"
  cd "$SRC/libelf-$LIBELF_VERSION"
  apply . "$(bs patches/libelf-configure-in.patch)"
  LIBTOOLIZE=glibtoolize autoreconf -i
  cp "$(bs config.guess)" "$(bs config.sub)" .
  ./configure --build="$TRIPLE" --host="$TRIPLE" --prefix="$DEPS" --libdir="$DEPS/lib" --disable-shared --disable-nls
  make
  make install
}

b_libyaml() {
  unpack "yaml-$LIBYAML_VERSION.tar.gz"
  cd "$SRC/yaml-$LIBYAML_VERSION"
  cp "$(bs config.guess)" "$(bs config.sub)" config/
  ./configure --build="$TRIPLE" --host="$TRIPLE" --prefix="$DEPS" --libdir="$DEPS/lib" --disable-shared --enable-static "CFLAGS=$OPT -DYAML_DECLARE_STATIC"
  make -j"$NCPU"
  make install
}

b_gmp() {
  unpack "gmp-$GMP_VERSION.tar.bz2"
  cd "$SRC/gmp-$GMP_VERSION"
  # --build=--host: no CPU-specific code (see the header of this file); --with-pic: the x86_64 assembly of GMP otherwise has text
  # relocations, which Apple's linker refuses in a position-independent executable.
  CFLAGS="-std=gnu99 $OPT" CPPFLAGS=-fexceptions ./configure --build="$TRIPLE" --host="$TRIPLE" --prefix="$DEPS" --libdir="$DEPS/lib" \
    --enable-cxx --disable-shared --enable-static --with-pic
  make -j"$NCPU"
  make install
  grep -E '^(HAVE_HOST_CPU|GMP_.*CPU|CPUVEC)' config.log | head -5 || true
}

b_mpfr() {
  unpack "mpfr-$MPFR_VERSION.tar.bz2"
  cd "$SRC/mpfr-$MPFR_VERSION"
  ./configure --build="$TRIPLE" --host="$TRIPLE" --prefix="$DEPS" --libdir="$DEPS/lib" --with-gmp="$DEPS" --disable-shared --enable-static
  make -j"$NCPU"
  make install
}

b_mpc() {
  unpack "mpc-$MPC_VERSION.tar.gz"
  cd "$SRC/mpc-$MPC_VERSION"
  ./configure --build="$TRIPLE" --host="$TRIPLE" --prefix="$DEPS" --libdir="$DEPS/lib" --with-gmp="$DEPS" --with-mpfr="$DEPS" \
    --disable-shared --enable-static --disable-nls
  make -j"$NCPU"
  make install
}

b_isl() {
  unpack "isl-$ISL_VERSION.tar.bz2"
  cd "$SRC/isl-$ISL_VERSION"
  ./configure --build="$TRIPLE" --host="$TRIPLE" --prefix="$DEPS" --libdir="$DEPS/lib" --with-gmp-prefix="$DEPS" --disable-shared --enable-static
  make -j"$NCPU"
  make install
}

b_binutils() {                # prefix "/" with DESTDIR=PREFIX, as the SDK: the programs find their scripts relative to themselves
  unpack "binutils-$BINUTILS_VERSION.tar.xz"
  local s=$SRC/binutils-$BINUTILS_VERSION
  apply "$s" "$(bs patches/binutils/0001-vita.patch)" "$(bs patches/binutils/0002-fix-broken-reloc.patch)" \
    "$(bs patches/binutils/0003-fix-elf-vaddr.patch)" "$(bs patches/binutils/0004-fix-interworking-veneers.patch)" \
    "$(bs patches/binutils/0005-genscripts-mkdir.patch)"
  cp "$(bs config.guess)" "$(bs config.sub)" "$s/"
  mkdir -p "$BLD/binutils"; cd "$BLD/binutils"
  CFLAGS="-std=gnu11 $OPT" "$s/configure" --build="$TRIPLE" --host="$TRIPLE" --target="$TARGET_TRIPLE" --prefix=/ --with-sysroot="$PREFIX" \
    --disable-nls --disable-werror --disable-separate-code --enable-interwork --enable-plugins --without-zstd "--with-pkgversion=$PKGVERSION"
  make -j"$NCPU" INFO_DEPS= DVIS= pdfs= htmls=
  make install DESTDIR="$PREFIX" INFO_DEPS= DVIS= pdfs= htmls=
}

b_vita_toolchain() {          # only the two programs the kit uses; the cmake options are the recipe's
  local s=$SRC/vita-toolchain
  cmake -S "$s" -B "$BLD/vita-toolchain" "${CMAKE_COMMON[@]}" -DCMAKE_INSTALL_PREFIX=/ -DUSE_BUNDLED_ENDIAN_H=ON -DTOOLCHAIN_DEPS_DIR="$DEPS" \
    -Dlibelf_LIBRARY="$DEPS/lib/libelf.a" -Dzlib_LIBRARY="$DEPS/lib/libz.a" -Dlibzip_LIBRARY="$DEPS/lib/libzip.a" -Dlibyaml_LIBRARY="$DEPS/lib/libyaml.a"
  cmake --build "$BLD/vita-toolchain" -j "$NCPU" --target vita-elf-create vita-make-fself
  mkdir -p "$PREFIX/bin"
  cp "$BLD/vita-toolchain/src/vita-elf-create" "$BLD/vita-toolchain/src/vita-make-fself" "$PREFIX/bin/"
}

b_licenses() {                # licence texts of the libraries linked into the programs (the sources are not kept in the cache)
  mkdir -p "$ROOT/licenses"
  lic() {                     # lic NAME DIR FILE...: the first of the files that exists
    local name=$1 dir=$2; shift 2
    local f
    for f in "$@"; do if [ -f "$dir/$f" ]; then cp "$dir/$f" "$ROOT/licenses/host-$name-$f.txt"; echo "licence $name: $f"; return 0; fi; done
    echo "no licence file for $name in $dir" >&2; return 1
  }
  lic gmp "$SRC/gmp-$GMP_VERSION" COPYING.LESSERv3
  lic mpfr "$SRC/mpfr-$MPFR_VERSION" COPYING.LESSER
  lic mpc "$SRC/mpc-$MPC_VERSION" COPYING.LESSER
  lic isl "$SRC/isl-$ISL_VERSION" LICENSE
  lic libelf "$SRC/libelf-$LIBELF_VERSION" COPYING.LIB
  lic libyaml "$SRC/yaml-$LIBYAML_VERSION" License LICENSE
  lic libzip "$SRC/libzip-$LIBZIP_VERSION" LICENSE
  lic zlib "$SRC/zlib-$ZLIB_VERSION" LICENSE README
}

phase_deps() {
  for t in cmake autoconf automake git curl; do need_tool "$t" || { echo "missing tool: $t" >&2; exit 1; }; done
  need_tool glibtoolize || need_tool libtoolize || { echo "missing tool: glibtoolize (brew install libtool)" >&2; exit 1; }
  step fetch do_fetch_all
  step zlib b_zlib
  step libzip b_libzip
  step libelf b_libelf
  step libyaml b_libyaml
  step gmp b_gmp
  step mpfr b_mpfr
  step mpc b_mpc
  step isl b_isl
  step binutils b_binutils
  step vita-toolchain b_vita_toolchain
  step licenses b_licenses
  touch "$ROOT/.deps-done"
}

# ------------------------------------------------------------------------------------------------------------------ phase gcc
gcc_env() {                   # BuildGccFinal.cmake: the binutils of the SDK being built answer gcc's configure tests of the assembler
  export AR_FOR_TARGET=$PREFIX/bin/$TARGET_TRIPLE-ar LD_FOR_TARGET=$PREFIX/bin/$TARGET_TRIPLE-ld OBJDUMP_FOR_TARGET=$PREFIX/bin/$TARGET_TRIPLE-objdump
  export NM_FOR_TARGET=$PREFIX/bin/$TARGET_TRIPLE-nm RANLIB_FOR_TARGET=$PREFIX/bin/$TARGET_TRIPLE-ranlib READELF_FOR_TARGET=$PREFIX/bin/$TARGET_TRIPLE-readelf
  export STRIP_FOR_TARGET=$PREFIX/bin/$TARGET_TRIPLE-strip AS_FOR_TARGET=$PREFIX/bin/$TARGET_TRIPLE-as
}

b_gcc_configure() {
  fetch_patches
  # shellcheck disable=SC2046
  fetch "gcc-$GCC_VERSION.tar.xz" "$GCC_SHA256" $(gnu "gcc/gcc-$GCC_VERSION" "gcc-$GCC_VERSION.tar.xz")
  unpack "gcc-$GCC_VERSION.tar.xz"
  local s=$SRC/gcc-$GCC_VERSION
  apply "$s" "$(bs patches/gcc/0001-vita-target.patch)" "$(bs patches/gcc/0002-vita-driver.patch)" \
    "$(bs patches/gcc/0003-libgomp-vita.patch)" "$(bs patches/gcc/0004-host-compat.patch)"
  # configure looks for the target headers and the assembler in the sysroot of the SDK; the headers are not needed for the host programs
  mkdir -p "$PREFIX/$TARGET_TRIPLE/include" "$PREFIX/$TARGET_TRIPLE/lib"
  for t in as ld ar nm readelf; do test -x "$PREFIX/bin/$TARGET_TRIPLE-$t" || { echo "binutils phase left no $TARGET_TRIPLE-$t" >&2; exit 1; }; done
  gcc_env
  mkdir -p "$BLD/gcc"; cd "$BLD/gcc"
  local f="$OPT -fbracket-depth=512"        # GCC_CFLAGS of GccCommonArgs.cmake (clang on Darwin), plus our optimisation
  "$s/configure" --build="$TRIPLE" --host="$TRIPLE" --target="$TARGET_TRIPLE" --prefix="$PREFIX" --libdir="$PREFIX/lib" --libexecdir="$PREFIX/lib" \
    --with-sysroot="$PREFIX/$TARGET_TRIPLE" --with-gmp="$DEPS" --with-mpfr="$DEPS" --with-mpc="$DEPS" --with-isl="$DEPS" --with-libelf="$DEPS" \
    --with-python-dir=share/gcc-$TARGET_TRIPLE --enable-languages=c,c++ --disable-decimal-float --disable-libcc1 --disable-libffi \
    --disable-libmudflap --disable-libquadmath --disable-libssp --disable-libstdcxx-pch --disable-nls --disable-shared --disable-tls \
    --with-gnu-as --with-gnu-ld --with-newlib --without-zstd --disable-multilib --with-arch=armv7-a --with-tune=cortex-a9 --with-fpu=neon \
    --with-float=hard --with-mode=thumb "--with-pkgversion=$PKGVERSION" --with-headers=yes --enable-threads=posix --enable-libgomp \
    "CFLAGS=$f" "CXXFLAGS=$f"
}

b_gcc_make() {
  gcc_env
  cd "$BLD/gcc"
  make -j"$NCPU" all-gcc all-lto-plugin INHIBIT_LIBC_CFLAGS="-DUSE_TM_CLONE_REGISTRY=0"
}

b_gcc_install() {
  gcc_env
  cd "$BLD/gcc"
  make install-gcc install-lto-plugin
  # PublishBfdPlugins.cmake: ar, nm and ld look for GCC's LTO plugin in lib/bfd-plugins
  local gl=$PREFIX/lib/gcc/$TARGET_TRIPLE/$GCC_VERSION
  local p=""
  for c in "$gl/liblto_plugin.so" "$gl/liblto_plugin.dylib"; do [ -f "$c" ] && p=$c && break; done
  [ -n "$p" ] || { echo "gcc built no loadable LTO plugin in $gl" >&2; ls "$gl"; exit 1; }
  mkdir -p "$PREFIX/lib/bfd-plugins"
  cp "$p" "$PREFIX/lib/bfd-plugins/"
}

# the files of the host SDK the kit takes (kit/lists/host-files.txt): prog = executable, lib = plugin (.so or .dylib)
b_stage() {
  rm -rf "$OUT"; mkdir -p "$OUT"
  local kind path got
  while read -r kind path _; do
    case "$kind" in prog|lib) ;; *) continue;; esac
    got=""
    if [ "$kind" = prog ]; then
      [ -f "$PREFIX/$path" ] && got=$path
    else
      for c in "$path.dylib" "$path.so"; do [ -f "$PREFIX/$c" ] && got=$c && break; done
    fi
    [ -n "$got" ] || { echo "the build left no $path" >&2; exit 1; }
    mkdir -p "$OUT/$(dirname "$got")"
    cp "$PREFIX/$got" "$OUT/$got"
    chmod 755 "$OUT/$got"
    echo "staged $got"
  done < <(sed 's/#.*//' "$HERE/../kit/lists/host-files.txt")
  mkdir -p "$OUT/licenses"
  cp "$ROOT"/licenses/* "$OUT/licenses/"
  cat > "$OUT/version_info.txt" <<EOF
macOS host programs of the Infected builder kit, built by CI (builder/hosttool/build-mac-host.sh) for macOS $TARGET or newer on $ARCH.
Same sources and configure options as the VitaSDK release sdk-snapshot-20260926.790.1 (https://github.com/vitasdk/autobuilds),
whose build recipe is vitasdk/buildscripts revision $BUILDSCRIPTS_REV; only the host programs were built.
world             vita (float-abi=hard)
gcc               $GCC_VERSION (patches/gcc/0001..0004 of that revision)
binutils          $BINUTILS_VERSION (patches/binutils/0001..0005 of that revision)
vita-toolchain    $VITA_TOOLCHAIN_COMMIT
gmp               $GMP_VERSION
mpfr              $MPFR_VERSION
mpc               $MPC_VERSION
isl               $ISL_VERSION
zlib              $ZLIB_VERSION
libelf            $LIBELF_VERSION
libyaml           $LIBYAML_VERSION
libzip            $LIBZIP_VERSION
compiler          $(cc --version | head -1)
deployment target $TARGET
EOF
  du -sh "$OUT"
}

phase_gcc() {
  [ -f "$ROOT/.deps-done" ] || { echo "the deps phase has not been done in $ROOT" >&2; exit 1; }
  step gcc-configure b_gcc_configure
  step gcc-make b_gcc_make
  step gcc-install b_gcc_install
  step stage b_stage
}

case "$PHASE" in
  deps) phase_deps;;
  gcc) phase_gcc;;
  all) phase_deps; phase_gcc;;
esac

# Warnings of the compiler about calls that the lowest macOS does not have are the first sign of a program that will not start there.
if grep -l -E "unguarded-availability|is only available on macOS|introduced in macOS" "$LOGS"/*.log 2>/dev/null; then
  echo "WARNING: the logs above mention functions newer than macOS $TARGET:"
  grep -h -E "unguarded-availability|is only available on macOS|introduced in macOS" "$LOGS"/*.log | sort | uniq -c | sort -rn | head -40
  touch "$LOGS/AVAILABILITY-WARNINGS"
fi
echo "---- timings (seconds)"; cat "$TIMINGS"
