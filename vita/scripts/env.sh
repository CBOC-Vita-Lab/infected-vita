# Source this: toolchain + private sysroot for the Vita port workstream.
export VITASDK=${VITASDK:?set VITASDK to the root of the Vita SDK (docs/BUILDING.md)}
export PATH=$VITASDK/bin:$PATH
VITA_WS=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export VITA_WS
export VITA_SYSROOT=$VITA_WS/sysroot          # our libs/headers (never touches the shared SDK)
export PKG_CONFIG_PATH=$VITA_SYSROOT/lib/pkgconfig
