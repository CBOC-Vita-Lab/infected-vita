// Minimal zlib API for the Vita build: PortableKit's GE dump reader needs uncompress() (host/gpu/ge_dump.cpp) and the SDK has no libz.
// uncompress() is a complete inflate (stored, fixed and dynamic blocks; the Adler-32 trailer is verified). compress2() does not compress:
// it reports Z_BUF_ERROR, which makes the recorder store pages raw, so a dump recorded ON the Vita is readable anywhere.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char Bytef;
typedef unsigned long uLongf;
typedef unsigned long uLong;

#define Z_OK 0
#define Z_BUF_ERROR (-5)
#define Z_DATA_ERROR (-3)

int uncompress(Bytef* dest, uLongf* destLen, const Bytef* source, uLong sourceLen);
int compress2(Bytef* dest, uLongf* destLen, const Bytef* source, uLong sourceLen, int level);
uLong compressBound(uLong sourceLen);

#ifdef __cplusplus
}
#endif
