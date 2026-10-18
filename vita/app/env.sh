# Source this: toolchain + sysroot for the Infected Vita application build.
#   VITASDK       the root of the Vita SDK (required; docs/BUILDING.md)
#   VITA_SYSROOT  the libraries that vita/scripts build (default: vita/sysroot of this repository)
export VITASDK=${VITASDK:?set VITASDK to the root of the Vita SDK (docs/BUILDING.md)}
export PATH=$VITASDK/bin:$PATH
export VITA_SYSROOT=${VITA_SYSROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/sysroot}
