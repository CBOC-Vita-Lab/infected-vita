package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"os"
)

// stripVelf makes the Sony ELF (vita-elf-create's output) independent of everything the Vita does not load: only the ELF header, the
// program headers and the bytes of the segments stay; everything else (section contents, section headers, the tail of the file) is
// dropped and the section header fields of the ELF header are zeroed.
//
// Why: vita-make-fself takes the module NID from a hash of the whole file, and the file carried the linker's symbol table
// (.symtab, .strtab, .rel.*), whose order is the order of ld's hash table. ld's hash is an `unsigned long`, 64 bits wide on Linux and
// macOS and 32 on Windows, so the symbols with long C++ names change places there, the NID differs, and so does eboot.bin, although every
// segment is the same. (vita-elf-create -s does the same on Linux and macOS but ignores -s on Windows.) The kit asks for this with
// "stripSections": true in the elfCreate recipe.
func stripVelf(path string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	out, err := stripELF32(data)
	if err != nil {
		return fmt.Errorf("%s: %v", path, err)
	}
	tmp := path + ".strip"
	if err := os.WriteFile(tmp, out, 0o644); err != nil {
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

func stripELF32(data []byte) ([]byte, error) {
	le := binary.LittleEndian
	if len(data) < 52 || string(data[:4]) != "\x7fELF" || data[4] != 1 || data[5] != 1 {
		return nil, errors.New("not a 32-bit little-endian ELF file")
	}
	phoff, phentsize, phnum := uint64(le.Uint32(data[28:])), uint64(le.Uint16(data[42:])), uint64(le.Uint16(data[44:]))
	if phnum == 0 || phentsize < 32 || phoff+phnum*phentsize > uint64(len(data)) {
		return nil, errors.New("the program headers are missing or outside the file")
	}
	type span struct{ from, to uint64 }
	keep := []span{{0, 52}, {phoff, phoff + phnum*phentsize}}
	for i := uint64(0); i < phnum; i++ {
		p := data[phoff+i*phentsize:]
		off, filesz := uint64(le.Uint32(p[4:])), uint64(le.Uint32(p[16:]))
		if filesz == 0 {
			continue
		}
		if off+filesz > uint64(len(data)) {
			return nil, fmt.Errorf("segment %d ends outside the file", i)
		}
		keep = append(keep, span{off, off + filesz})
	}
	var end uint64
	for _, k := range keep {
		if k.to > end {
			end = k.to
		}
	}
	out := make([]byte, end)
	for _, k := range keep {
		copy(out[k.from:k.to], data[k.from:k.to])
	}
	le.PutUint32(out[32:], 0) // e_shoff
	le.PutUint16(out[46:], 0) // e_shentsize
	le.PutUint16(out[48:], 0) // e_shnum
	le.PutUint16(out[50:], 0) // e_shstrndx
	return out, nil
}
