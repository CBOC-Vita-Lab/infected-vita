// Symbols the desktop build gets from cmake/PortableKit.cmake or from tools: none of them has anything to do on the Vita.
#include "hle/hle_extensions.hpp"

namespace portablekit {

// No HLE extension modules are linked into the Vita build.
std::span<const HleExtensionModule> linked_hle_extensions() { return {}; }

} // namespace portablekit
