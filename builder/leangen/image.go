// image.go mirrors recomp/leangen/image.py: loader for the relocated Infected image + PRX metadata (relocation sites, module info,
// import stubs).
package leangen

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"strings"
)

// Base is the address the image is loaded at.
const Base = 0x08804000

const (
	rMips32, rMips26, rMipsHi16, rMipsLo16 = 2, 4, 5, 6
)

type elfSegment struct{ typ, off, vaddr, paddr, filesz, memsz uint32 }

type elfSection struct {
	name                                     string
	nameOff, typ, addr, off, size, link, inf uint32
}

// Reloc is one relocation site: (addr, type, patch segment, target segment).
type Reloc struct {
	Addr uint32
	Typ  uint32
	PS   uint32
	TS   uint32
}

// Import is one import stub: library name and NID.
type Import struct {
	Lib string
	NID uint32
}

// pointer source kinds of code_pointers()
const (
	ptrHilo   = 1
	ptrData32 = 2
	ptrText32 = 4
)

// Image is the relocated load image of the game's PRX plus the metadata the generator needs.
type Image struct {
	img       []byte
	Base      uint32
	segments  []elfSegment
	sections  []elfSection
	EntryOff  uint32
	TextStart uint32
	TextEnd   uint32
	StubStart uint32
	StubEnd   uint32
	CodeEnd   uint32
	NWords    int
	Words     []uint32
	SegAddr   []uint32
	Relocs    []Reloc
	Gp        uint32
	EntTop    uint32
	EntEnd    uint32
	StubTop   uint32
	StubEndT  uint32
	Imports   map[uint32]Import
	Entry     uint32
	// PtrSrc: code pointer value -> addresses of the R_MIPS_32 words holding it (filled by CodePointers)
	PtrSrc map[uint32][]uint32
}

// NewImage parses boot (BOOT.BIN, the PRX) and image (its relocated load image, see psp.BuildImage).
func NewImage(image, boot []byte) (im *Image, err error) {
	defer func() {
		if r := recover(); r != nil {
			im, err = nil, fmt.Errorf("this does not look like the game's BOOT.BIN: %v", r)
		}
	}()
	le := binary.LittleEndian
	im = &Image{img: image, Base: Base}
	if len(boot) < 52 {
		return nil, errors.New("BOOT.BIN is too short")
	}
	ePhoff := le.Uint32(boot[28:])
	eShoff := le.Uint32(boot[32:])
	ePhnum := int(le.Uint16(boot[44:]))
	eShnum := int(le.Uint16(boot[48:]))
	eShstrndx := int(le.Uint16(boot[50:]))
	im.EntryOff = le.Uint32(boot[24:])
	for i := 0; i < ePhnum; i++ {
		o := int(ePhoff) + 32*i
		im.segments = append(im.segments, elfSegment{le.Uint32(boot[o:]), le.Uint32(boot[o+4:]), le.Uint32(boot[o+8:]), le.Uint32(boot[o+12:]),
			le.Uint32(boot[o+16:]), le.Uint32(boot[o+20:])})
	}
	secs := make([]elfSection, eShnum)
	for i := 0; i < eShnum; i++ {
		o := int(eShoff) + 40*i
		secs[i] = elfSection{nameOff: le.Uint32(boot[o:]), typ: le.Uint32(boot[o+4:]), addr: le.Uint32(boot[o+12:]), off: le.Uint32(boot[o+16:]),
			size: le.Uint32(boot[o+20:]), link: le.Uint32(boot[o+24:]), inf: le.Uint32(boot[o+28:])}
	}
	stroff := int(secs[eShstrndx].off)
	for i := range secs {
		s := &secs[i]
		st := stroff + int(s.nameOff)
		e := bytes.IndexByte(boot[st:], 0)
		if e < 0 {
			return nil, errors.New("unterminated section name")
		}
		s.name = string(boot[st : st+e])
	}
	im.sections = secs
	var text *elfSection
	for i := range secs {
		if secs[i].name == ".text" {
			text = &secs[i]
			break
		}
	}
	if text == nil {
		return nil, errors.New("no .text section")
	}
	im.TextStart = Base + text.addr
	im.TextEnd = Base + text.addr + text.size
	first := true
	var minA, maxE uint32
	for i := range secs {
		s := &secs[i]
		if !strings.HasPrefix(s.name, ".sceStub.text") {
			continue
		}
		if first || s.addr < minA {
			minA = s.addr
		}
		if first || s.addr+s.size > maxE {
			maxE = s.addr + s.size
		}
		first = false
	}
	if first {
		return nil, errors.New("no .sceStub.text section")
	}
	im.StubStart = Base + minA
	im.StubEnd = Base + maxE
	im.CodeEnd = im.StubEnd
	im.NWords = int(im.StubEnd-Base) / 4
	if im.NWords*4 > len(image) {
		return nil, errors.New("the image is shorter than the code range")
	}
	im.Words = make([]uint32, im.NWords)
	for i := range im.Words {
		im.Words[i] = le.Uint32(image[4*i:])
	}
	for _, s := range im.segments {
		im.SegAddr = append(im.SegAddr, Base+s.vaddr)
	}
	for i := range secs {
		s := &secs[i]
		if s.typ != 0x700000A0 {
			continue
		}
		for j := 0; j < int(s.size)/8; j++ {
			off := le.Uint32(boot[int(s.off)+8*j:])
			info := le.Uint32(boot[int(s.off)+8*j+4:])
			typ := info & 0xF
			ps := (info >> 8) & 0xFF
			ts := (info >> 16) & 0xFF
			if int(ps) >= len(im.SegAddr) || int(ts) >= len(im.SegAddr) {
				continue
			}
			im.Relocs = append(im.Relocs, Reloc{im.SegAddr[ps] + off, typ, ps, ts})
		}
	}
	var mi *elfSection
	for i := range secs {
		if secs[i].name == ".rodata.sceModuleInfo" {
			mi = &secs[i]
			break
		}
	}
	if mi == nil {
		return nil, errors.New("no module info section")
	}
	o := int(mi.addr)
	im.Gp = le.Uint32(image[o+32:])
	im.EntTop = le.Uint32(image[o+36:])
	im.EntEnd = le.Uint32(image[o+40:])
	im.StubTop = le.Uint32(image[o+44:])
	im.StubEndT = le.Uint32(image[o+48:])
	im.Imports = map[uint32]Import{}
	cur := im.StubTop
	for cur < im.StubEndT {
		libname := le.Uint32(image[cur-Base:])
		nwords := uint32(image[cur-Base+8])
		count := int(le.Uint16(image[cur-Base+10:]))
		nidt := le.Uint32(image[cur-Base+12:])
		stubt := le.Uint32(image[cur-Base+16:])
		lib := im.cstr(libname)
		for i := 0; i < count; i++ {
			nid := le.Uint32(image[nidt-Base+uint32(4*i):])
			im.Imports[stubt+uint32(8*i)] = Import{lib, nid}
		}
		step := nwords * 4
		if step < 20 {
			step = 20
		}
		cur += step
	}
	im.Entry = Base + im.EntryOff
	return im, nil
}

func (im *Image) cstr(a uint32) string {
	o := int(a - Base)
	e := bytes.IndexByte(im.img[o:], 0)
	return strings.ToValidUTF8(string(im.img[o:o+e]), "�")
}

// Word reads the image word at address a.
func (im *Image) Word(a uint32) uint32 { return binary.LittleEndian.Uint32(im.img[a-Base:]) }

// ImgLen is the size of the image in bytes.
func (im *Image) ImgLen() int { return len(im.img) }

// InCode reports whether a is a word-aligned address of the code range.
func (im *Image) InCode(a uint32) bool { return Base <= a && a < im.StubEnd && a&3 == 0 }

// InText reports whether a is a word-aligned address inside .text proper.
func (im *Image) InText(a uint32) bool { return im.TextStart <= a && a < im.TextEnd && a&3 == 0 }

// IsStub reports whether a is an import stub.
func (im *Image) IsStub(a uint32) bool { _, ok := im.Imports[a]; return ok }

// CodePointers ports code_pointers(): address-taken code pointers from the relocation table
// (-> addr -> set of source kinds: 'data32' / 'text32' R_MIPS_32 words, 'hilo' lui/addiu pairs).
func (im *Image) CodePointers() map[uint32]uint8 {
	out := map[uint32]uint8{}
	im.PtrSrc = map[uint32][]uint32{}
	rel := im.Relocs
	n := len(rel)
	for i := 0; i < n; i++ {
		a, t, ps, ts := rel[i].Addr, rel[i].Typ, rel[i].PS, rel[i].TS
		switch t {
		case rMips32:
			var v uint32
			if int(a-Base)+4 <= len(im.img) {
				v = im.Word(a)
			}
			if im.InCode(v) {
				if a >= im.StubEnd {
					out[v] |= ptrData32
				} else {
					out[v] |= ptrText32
				}
				im.PtrSrc[v] = append(im.PtrSrc[v], a)
			}
		case rMipsHi16:
			for j := i + 1; j < n; j++ {
				b, t2, ps2, ts2 := rel[j].Addr, rel[j].Typ, rel[j].PS, rel[j].TS
				if t2 == rMipsHi16 {
					continue
				}
				if t2 != rMipsLo16 || ps2 != ps || ts2 != ts {
					continue
				}
				hi := im.Word(a) & 0xFFFF
				lo := int64(im.Word(b) & 0xFFFF)
				if lo&0x8000 != 0 {
					lo -= 0x10000
				}
				v := uint32((int64(hi)<<16 + lo) & 0xFFFFFFFF)
				if im.InCode(v) {
					out[v] |= ptrHilo
				}
				break
			}
		}
	}
	return out
}
