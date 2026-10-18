// vita-elf-create appends the module's import/export tables ("SCE data", a few KiB) to the end of the code segment and fails
// ("Cannot allocate N bytes for SCE data at end of segment 0; segment 1 overlaps") when the data segment, which the linker script
// places on a 64 KiB boundary, starts less than that after the last code byte. How much room is left depends on the code size, so
// build.sh measures it and sets INFECTED_TEXT_PAD (bytes of filler in .text) when it is too little.
#ifndef INFECTED_TEXT_PAD
#define INFECTED_TEXT_PAD 0
#endif

#if INFECTED_TEXT_PAD > 0
extern "C" const unsigned char infected_text_pad[INFECTED_TEXT_PAD] __attribute__((used, section(".text.zz_pad"), aligned(4)));
extern "C" const unsigned char infected_text_pad[INFECTED_TEXT_PAD] = {0};
#endif
