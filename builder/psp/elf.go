// elf.go mirrors tools/dump_image.cpp: it loads a PSP PRX/ELF (BOOT.BIN) at a base address into 32 MB of guest RAM, applies the
// PSP relocations and writes out the words from the base to the end of the file-backed PT_LOAD data.
//
// The loader and relocator are translated from PortableKit's Elf32Image::load_and_relocate (src/elf32.cpp) and the RAM part of
// GuestMemory (src/guest_memory.cpp).
//
// PortableKit is distributed under the MIT License:
//
//	Copyright (c) 2026 PSPRecomp contributors
//	Copyright (c) 2026 PortableKit contributors
//
//	Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
//	files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
//	modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//	Software is furnished to do so, subject to the following conditions:
//
//	The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
//	Software.
//
//	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
//	WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
//	COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
//	ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
package psp

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// DefaultBase is the address the PSP loads a user PRX at (kDefaultPspUserLoadBase).
const DefaultBase = 0x08804000

const (
	elfTypePspPrx   = 0xFFA0
	sectionPspRel   = 0x700000A0
	ptLoad          = 1
	shtNobits       = 8
	rMips32         = 2
	rMips26         = 4
	rMipsHi16       = 5
	rMipsLo16       = 6
	ramPhysicalBase = 0x08000000
	ramSize32       = 32 * 1024 * 1024
)

type segment struct {
	typ, offset, vaddr, paddr, fileSize, memSize, flags, align uint32
}

type section struct {
	name                                          string
	typ, flags, addr, offset, size, link, info, a uint32
	entSize                                       uint32
}

// Elf is a parsed 32-bit little-endian PSP executable (Elf32Image).
type Elf struct {
	bytes    []byte
	typ      uint16
	entry    uint32
	segments []segment
	sections []section
}

// RelocationStats counts what the relocator did.
type RelocationStats struct {
	Total, Mips32, Mips26, MipsHi16, MipsLo16, Unsupported, Invalid uint32
}

func (e *Elf) span(off, length uint64) ([]byte, error) {
	if off > uint64(len(e.bytes)) || length > uint64(len(e.bytes))-off {
		return nil, errors.New("truncated ELF structure in the executable")
	}
	return e.bytes[off : off+length], nil
}

// ParseElf parses bytes as a PSP executable (Elf32Image::from_bytes).
func ParseElf(b []byte) (*Elf, error) {
	e := &Elf{bytes: b}
	if len(b) >= 4 && b[0] == '~' && b[1] == 'P' && b[2] == 'S' && b[3] == 'P' {
		return nil, errors.New("encrypted or compressed ~PSP executable: this is not a decrypted BOOT.BIN")
	}
	if len(b) >= 4 && b[0] == 0 && b[1] == 'P' && b[2] == 'B' && b[3] == 'P' {
		return nil, errors.New("PBP container found instead of BOOT.BIN")
	}
	h, err := e.span(0, 52)
	if err != nil {
		return nil, err
	}
	if h[0] != 0x7F || h[1] != 'E' || h[2] != 'L' || h[3] != 'F' {
		return nil, errors.New("not an ELF file")
	}
	if h[4] != 1 || h[5] != 1 {
		return nil, errors.New("ELF32 little-endian input required")
	}
	le := binary.LittleEndian
	e.typ = le.Uint16(h[16:])
	if le.Uint16(h[18:]) != 8 {
		return nil, errors.New("ELF machine is not MIPS/Allegrex")
	}
	e.entry = le.Uint32(h[24:])
	phoff, shoff := le.Uint32(h[28:]), le.Uint32(h[32:])
	phentsize, phnum := le.Uint16(h[42:]), le.Uint16(h[44:])
	shentsize, shnum, shstrndx := le.Uint16(h[46:]), le.Uint16(h[48:]), le.Uint16(h[50:])
	if phentsize != 0 && phentsize < 32 {
		return nil, errors.New("invalid ELF program header size")
	}
	if shentsize != 0 && shentsize < 40 {
		return nil, errors.New("invalid ELF section header size")
	}
	for i := 0; i < int(phnum); i++ {
		d, err := e.span(uint64(phoff)+uint64(i)*uint64(phentsize), 32)
		if err != nil {
			return nil, err
		}
		s := segment{le.Uint32(d[0:]), le.Uint32(d[4:]), le.Uint32(d[8:]), le.Uint32(d[12:]), le.Uint32(d[16:]), le.Uint32(d[20:]), le.Uint32(d[24:]), le.Uint32(d[28:])}
		if s.fileSize > 0 {
			if _, err := e.span(uint64(s.offset), uint64(s.fileSize)); err != nil {
				return nil, err
			}
		}
		e.segments = append(e.segments, s)
	}
	type rawSec struct{ name, typ, flags, addr, offset, size, link, info, align, entsize uint32 }
	raws := make([]rawSec, 0, shnum)
	for i := 0; i < int(shnum); i++ {
		d, err := e.span(uint64(shoff)+uint64(i)*uint64(shentsize), 40)
		if err != nil {
			return nil, err
		}
		var r rawSec
		f := []*uint32{&r.name, &r.typ, &r.flags, &r.addr, &r.offset, &r.size, &r.link, &r.info, &r.align, &r.entsize}
		for k, p := range f {
			*p = le.Uint32(d[4*k:])
		}
		if r.typ != shtNobits && r.size > 0 {
			if _, err := e.span(uint64(r.offset), uint64(r.size)); err != nil {
				return nil, err
			}
		}
		raws = append(raws, r)
	}
	var strtab []byte
	if int(shstrndx) < len(raws) {
		sh := raws[shstrndx]
		if strtab, err = e.span(uint64(sh.offset), uint64(sh.size)); err != nil {
			return nil, err
		}
	}
	for _, r := range raws {
		name := ""
		if len(strtab) > 0 && int(r.name) < len(strtab) {
			rest := strtab[r.name:]
			for k, c := range rest {
				if c == 0 {
					name = string(rest[:k])
					break
				}
			}
		}
		e.sections = append(e.sections, section{name, r.typ, r.flags, r.addr, r.offset, r.size, r.link, r.info, r.align, r.entsize})
	}
	return e, nil
}

// ram is the part of PortableKit's GuestMemory that the image dump uses: the main RAM block at 0x08000000.
type ram struct{ b []byte }

func canonical(a uint32) uint32 { return a & 0x1FFFFFFF }

func (m *ram) contains(addr uint32, length uint64) bool {
	c := canonical(addr)
	if c < ramPhysicalBase {
		return false
	}
	return uint64(c)+length <= uint64(ramPhysicalBase)+uint64(len(m.b))
}

func (m *ram) load32(addr uint32) uint32 {
	o := canonical(addr) - ramPhysicalBase
	return binary.LittleEndian.Uint32(m.b[o:])
}

func (m *ram) store32(addr, v uint32) {
	o := canonical(addr) - ramPhysicalBase
	binary.LittleEndian.PutUint32(m.b[o:], v)
}

func (e *Elf) isPrx() bool { return e.typ == elfTypePspPrx }

func (e *Elf) segmentAddress(i int, base uint32) uint32 {
	if e.isPrx() {
		return base + e.segments[i].vaddr
	}
	return e.segments[i].vaddr
}

func (e *Elf) loadInto(m *ram, base uint32) error {
	loaded := false
	for i, s := range e.segments {
		if s.typ != ptLoad {
			continue
		}
		if s.memSize < s.fileSize {
			return errors.New("ELF PT_LOAD memsz is smaller than filesz")
		}
		addr := e.segmentAddress(i, base)
		data, err := e.span(uint64(s.offset), uint64(s.fileSize))
		if err != nil {
			return err
		}
		if !m.contains(addr, uint64(len(data))) || !m.contains(addr, uint64(s.memSize)) {
			return fmt.Errorf("guest memory access outside PSP RAM at %08x", addr)
		}
		copy(m.b[canonical(addr)-ramPhysicalBase:], data)
		loaded = true
	}
	if !loaded {
		return errors.New("ELF contains no PT_LOAD segment")
	}
	return nil
}

func relocatedHi(value uint32) uint16 {
	signedLow := int16(value & 0xFFFF)
	corrected := value - uint32(int32(signedLow))
	return uint16((corrected >> 16) & 0xFFFF)
}

func (e *Elf) applyRelocations(m *ram, base uint32) (RelocationStats, error) {
	var st RelocationStats
	if !e.isPrx() {
		return st, nil
	}
	segAddr := make([]uint32, len(e.segments))
	for i, s := range e.segments {
		segAddr[i] = base + s.vaddr
	}
	le := binary.LittleEndian
	for _, sec := range e.sections {
		if sec.typ != sectionPspRel {
			continue
		}
		if sec.size%8 != 0 {
			return st, fmt.Errorf("malformed PSP relocation section %s", sec.name)
		}
		count := int(sec.size / 8)
		raw, err := e.span(uint64(sec.offset), uint64(sec.size))
		if err != nil {
			return st, err
		}
		offs := make([]uint32, count)
		infos := make([]uint32, count)
		orig := make([]uint32, count)
		valid := make([]bool, count)
		for i := 0; i < count; i++ {
			offs[i], infos[i] = le.Uint32(raw[8*i:]), le.Uint32(raw[8*i+4:])
			valid[i] = true
		}
		for i := 0; i < count; i++ {
			ps := (infos[i] >> 8) & 0xFF
			typ := infos[i] & 0x0F
			if int(ps) >= len(segAddr) {
				valid[i] = false
				st.Invalid++
				continue
			}
			pa := segAddr[ps] + offs[i]
			if !m.contains(pa, 4) || (pa&3 != 0 && typ != rMips32) {
				valid[i] = false
				st.Invalid++
				continue
			}
			orig[i] = m.load32(pa)
		}
		for i := 0; i < count; i++ {
			st.Total++
			if !valid[i] {
				continue
			}
			typ := infos[i] & 0x0F
			ps := (infos[i] >> 8) & 0xFF
			ts := (infos[i] >> 16) & 0xFF
			if int(ts) >= len(segAddr) {
				st.Invalid++
				continue
			}
			pa := segAddr[ps] + offs[i]
			to := segAddr[ts]
			op := orig[i]
			switch typ {
			case rMips32:
				op += to
				st.Mips32++
			case rMips26:
				op = (op & 0xFC000000) | (((op & 0x03FFFFFF) + (to >> 2)) & 0x03FFFFFF)
				st.Mips26++
			case rMipsHi16:
				paired := false
				identity := infos[i] >> 8
				for j := i + 1; j < count; j++ {
					ct := infos[j] & 0x0F
					if ct == rMipsHi16 {
						continue
					}
					if ct != rMipsLo16 || (infos[j]>>8) != identity || !valid[j] {
						continue
					}
					low := int16(orig[j] & 0xFFFF)
					value := (op & 0xFFFF) << 16
					value += uint32(int32(low))
					value += to
					op = (op & 0xFFFF0000) | uint32(relocatedHi(value))
					paired = true
					break
				}
				if !paired {
					st.Invalid++
					continue
				}
				st.MipsHi16++
			case rMipsLo16:
				op = (op & 0xFFFF0000) | ((op + to) & 0xFFFF)
				st.MipsLo16++
			default:
				st.Unsupported++
				continue
			}
			m.store32(pa, op)
		}
	}
	return st, nil
}

// RelocatedImage loads the executable at base into 32 MB of guest RAM, relocates it and returns the words from base to the end
// of the file-backed PT_LOAD data (tools/dump_image.cpp).
func (e *Elf) RelocatedImage(base uint32) ([]byte, RelocationStats, error) {
	m := &ram{b: make([]byte, ramSize32)}
	if err := e.loadInto(m, base); err != nil {
		return nil, RelocationStats{}, err
	}
	st, err := e.applyRelocations(m, base)
	if err != nil {
		return nil, st, err
	}
	var end uint32
	for i, s := range e.segments {
		if s.typ != ptLoad {
			continue
		}
		if x := e.segmentAddress(i, base) + s.fileSize; x > end {
			end = x
		}
	}
	if end < base {
		return nil, st, errors.New("executable has no data")
	}
	n := (uint64(end-base) + 3) / 4
	if canonical(base)-ramPhysicalBase+uint32(n*4) > uint32(len(m.b)) {
		return nil, st, errors.New("executable does not fit in guest RAM")
	}
	o := canonical(base) - ramPhysicalBase
	out := make([]byte, n*4)
	copy(out, m.b[o:])
	return out, st, nil
}

// BuildImage is the library form of dump_image: parse boot (the bytes of BOOT.BIN) and return its relocated load image at
// DefaultBase.
func BuildImage(boot []byte) ([]byte, error) {
	e, err := ParseElf(boot)
	if err != nil {
		return nil, err
	}
	img, _, err := e.RelocatedImage(DefaultBase)
	return img, err
}
