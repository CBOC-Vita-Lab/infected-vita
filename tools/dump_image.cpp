// Writes the relocated load image of an ELF/PRX (as loaded at 0x08804000) to a flat file.
// Build (needs only PortableKit's elf32.cpp and guest_memory.cpp, see docs/BUILDING.md):
//   g++ -std=c++20 -O1 -I$PK/include tools/dump_image.cpp $PK/src/elf32.cpp $PK/src/guest_memory.cpp -o dump_image
#include "psprecomp/elf32.hpp"
#include "psprecomp/guest_memory.hpp"
#include <cstdio>
#include <cstdlib>
int main(int argc, char **argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: dump_image elf out.bin [base]\n"); return 2; }
    std::uint32_t base = argc > 3 ? std::strtoul(argv[3], nullptr, 0) : 0x08804000u;
    auto elf = psprecomp::Elf32Image::from_file(argv[1]);
    psprecomp::GuestMemory mem(32u * 1024u * 1024u);
    (void)elf.load_and_relocate(mem, base);
    // code segment only: through the end of segment 0 (file-backed text) and data segment filesz
    std::uint32_t end = 0;
    for (std::size_t i = 0; i < elf.segments().size(); ++i) {
        const auto &s = elf.segments()[i];
        if (s.type != 1u) continue;
        std::uint32_t e = elf.segment_runtime_address(i, base) + s.file_size;
        if (e > end) end = e;
    }
    FILE *f = std::fopen(argv[2], "wb");
    for (std::uint32_t a = base; a < end; a += 4) { std::uint32_t w = mem.load32(a); std::fwrite(&w, 4, 1, f); }
    std::fclose(f);
    std::printf("wrote %u bytes (base %08x end %08x)\n", end - base, base, end);
}
namespace psprecomp {
std::int32_t runtime_thread_uid() noexcept { return 0; }
const char *runtime_thread_name() noexcept { return ""; }
std::uint32_t runtime_dispatch_pc() noexcept { return 0; }
void runtime_watch_context(std::uint32_t &pc, std::uint32_t &ra, std::uint32_t &sp) noexcept { pc = ra = sp = 0; }
}
