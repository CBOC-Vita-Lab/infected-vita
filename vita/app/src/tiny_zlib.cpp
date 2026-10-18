// tiny_zlib.cpp - uncompress() for zlib_shim/zlib.h: RFC 1950 wrapper around an RFC 1951 inflate (canonical Huffman by length counts,
// the method of Mark Adler's puff.c).
#include <stdint.h>
#include <string.h>

#include "zlib_shim/zlib.h"

namespace {

struct Stream {
    const uint8_t* in;
    size_t in_len, in_pos;
    uint8_t* out;
    size_t out_len, out_pos;
    uint32_t bitbuf;
    int bitcnt;
    bool error;
};

struct Huffman {
    uint16_t count[16];
    uint16_t symbol[288];
};

int bits(Stream& s, int need) {
    uint32_t val = s.bitbuf;
    while (s.bitcnt < need) {
        if (s.in_pos >= s.in_len) { s.error = true; return 0; }
        val |= (uint32_t)s.in[s.in_pos++] << s.bitcnt;
        s.bitcnt += 8;
    }
    s.bitbuf = val >> need;
    s.bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

int decode(Stream& s, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= bits(s, 1);
        if (s.error) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

int construct(Huffman& h, const uint16_t* length, int n) {
    memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; i++) h.count[length[i]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h.count[len];
    for (int i = 0; i < n; i++)
        if (length[i] != 0) h.symbol[offs[length[i]]++] = (uint16_t)i;
    return left;
}

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint16_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint16_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Stream& s, const Huffman& lencode, const Huffman& distcode) {
    for (;;) {
        int sym = decode(s, lencode);
        if (sym < 0) return false;
        if (sym < 256) {
            if (s.out_pos >= s.out_len) return false;
            s.out[s.out_pos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int len = kLenBase[sym] + bits(s, kLenExtra[sym]);
            int ds = decode(s, distcode);
            if (ds < 0 || ds >= 30) return false;
            size_t dist = kDistBase[ds] + (size_t)bits(s, kDistExtra[ds]);
            if (s.error || dist > s.out_pos || s.out_pos + len > s.out_len) return false;
            for (int i = 0; i < len; i++, s.out_pos++) s.out[s.out_pos] = s.out[s.out_pos - dist];
        }
    }
}

bool fixed(Stream& s) {
    static Huffman lencode, distcode;
    static bool built = false;
    if (!built) {
        uint16_t lengths[288];
        int i = 0;
        for (; i < 144; i++) lengths[i] = 8;
        for (; i < 256; i++) lengths[i] = 9;
        for (; i < 280; i++) lengths[i] = 7;
        for (; i < 288; i++) lengths[i] = 8;
        construct(lencode, lengths, 288);
        for (i = 0; i < 30; i++) lengths[i] = 5;
        construct(distcode, lengths, 30);
        built = true;
    }
    return codes(s, lencode, distcode);
}

bool dynamic(Stream& s) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (s.error || nlen > 286 || ndist > 30) return false;
    uint16_t lengths[320];
    int index = 0;
    for (; index < ncode; index++) lengths[order[index]] = (uint16_t)bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    Huffman lencode, distcode;
    if (construct(lencode, lengths, 19) != 0) return false;
    index = 0;
    while (index < nlen + ndist) {
        int sym = decode(s, lencode);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[index++] = (uint16_t)sym;
        } else {
            int len = 0, rep;
            if (sym == 16) {
                if (index == 0) return false;
                len = lengths[index - 1];
                rep = 3 + bits(s, 2);
            } else if (sym == 17) {
                rep = 3 + bits(s, 3);
            } else {
                rep = 11 + bits(s, 7);
            }
            if (index + rep > nlen + ndist) return false;
            while (rep--) lengths[index++] = (uint16_t)len;
        }
    }
    if (s.error || lengths[256] == 0) return false;
    int err = construct(lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return false;
    err = construct(distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return false;
    return codes(s, lencode, distcode);
}

bool inflate(Stream& s) {
    int last;
    do {
        last = bits(s, 1);
        int type = bits(s, 2);
        if (s.error) return false;
        if (type == 0) {
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.in_pos + 4 > s.in_len) return false;
            unsigned len = s.in[s.in_pos] | (s.in[s.in_pos + 1] << 8);
            unsigned nlen = s.in[s.in_pos + 2] | (s.in[s.in_pos + 3] << 8);
            s.in_pos += 4;
            if (len != (~nlen & 0xFFFF) || s.in_pos + len > s.in_len || s.out_pos + len > s.out_len) return false;
            memcpy(s.out + s.out_pos, s.in + s.in_pos, len);
            s.in_pos += len;
            s.out_pos += len;
        } else if (type == 1) {
            if (!fixed(s)) return false;
        } else if (type == 2) {
            if (!dynamic(s)) return false;
        } else {
            return false;
        }
    } while (!last);
    return true;
}

uint32_t adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

}  // namespace

extern "C" {

int uncompress(Bytef* dest, uLongf* destLen, const Bytef* source, uLong sourceLen) {
    if (sourceLen < 6 || (source[0] & 0x0F) != 8 || ((source[0] << 8) | source[1]) % 31 != 0 || (source[1] & 0x20)) return Z_DATA_ERROR;
    Stream s{source + 2, sourceLen - 2, 0, dest, *destLen, 0, 0, 0, false};
    if (!inflate(s)) return s.out_pos >= s.out_len ? Z_BUF_ERROR : Z_DATA_ERROR;
    if (s.in_pos + 4 <= s.in_len) {
        const uint8_t* t = s.in + s.in_pos;
        uint32_t want = (uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3];
        if (want != adler32(dest, s.out_pos)) return Z_DATA_ERROR;
    }
    *destLen = s.out_pos;
    return Z_OK;
}

int compress2(Bytef*, uLongf*, const Bytef*, uLong, int) { return Z_BUF_ERROR; }
uLong compressBound(uLong n) { return n + (n >> 12) + (n >> 14) + (n >> 25) + 13; }

}
