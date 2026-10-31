package psp

import (
	"bytes"
	"encoding/binary"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

// dirRecord builds an ISO9660 directory record.
func dirRecord(name string, lba, size uint32, isDir bool) []byte {
	n := []byte(name)
	l := 33 + len(n)
	if l%2 == 1 {
		l++
	}
	rec := make([]byte, l)
	rec[0] = byte(l)
	binary.LittleEndian.PutUint32(rec[2:], lba)
	binary.BigEndian.PutUint32(rec[6:], lba)
	binary.LittleEndian.PutUint32(rec[10:], size)
	binary.BigEndian.PutUint32(rec[14:], size)
	if isDir {
		rec[25] = 2
	}
	rec[32] = byte(len(n))
	copy(rec[33:], n)
	return rec
}

// syntheticISO makes a tiny image with PSP_GAME/SYSDIR/BOOT.BIN;1 (3000 bytes, so it spans two sectors) and PSP_GAME/PARAM.SFO;1.
func syntheticISO(boot []byte) []byte {
	const (
		rootLBA, gameLBA, sysLBA, bootLBA = 20, 21, 22, 24
	)
	img := make([]byte, (bootLBA+4)*SectorSize)
	pvd := img[16*SectorSize:]
	pvd[0] = 1
	copy(pvd[1:], "CD001")
	copy(pvd[156:], dirRecord("\x00", rootLBA, SectorSize, true))
	put := func(lba int, recs ...[]byte) {
		o := lba * SectorSize
		for _, r := range recs {
			copy(img[o:], r)
			o += len(r)
		}
	}
	put(rootLBA, dirRecord("\x00", rootLBA, SectorSize, true), dirRecord("\x01", rootLBA, SectorSize, true),
		dirRecord("PSP_GAME", gameLBA, SectorSize, true))
	put(gameLBA, dirRecord("\x00", gameLBA, SectorSize, true), dirRecord("\x01", rootLBA, SectorSize, true),
		dirRecord("SYSDIR", sysLBA, SectorSize, true), dirRecord("PARAM.SFO;1", 23, 5, false))
	put(sysLBA, dirRecord("\x00", sysLBA, SectorSize, true), dirRecord("\x01", gameLBA, SectorSize, true),
		dirRecord("BOOT.BIN;1", bootLBA, uint32(len(boot)), false))
	copy(img[23*SectorSize:], "hello")
	copy(img[bootLBA*SectorSize:], boot)
	return img
}

func TestReadFileSynthetic(t *testing.T) {
	boot := make([]byte, 3000)
	for i := range boot {
		boot[i] = byte(i * 7)
	}
	dir := t.TempDir()
	iso := filepath.Join(dir, "disc.iso")
	if err := os.WriteFile(iso, syntheticISO(boot), 0o644); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"PSP_GAME/SYSDIR/BOOT.BIN", "psp_game/sysdir/boot.bin", "/PSP_GAME/SYSDIR/BOOT.BIN"} {
		got, err := ReadFile(iso, name)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if !bytes.Equal(got, boot) {
			t.Fatalf("%s: wrong contents", name)
		}
	}
	got, err := ReadFile(iso, "PSP_GAME/PARAM.SFO")
	if err != nil || string(got) != "hello" {
		t.Fatalf("PARAM.SFO: %q %v", got, err)
	}
	if _, err := ReadFile(iso, "PSP_GAME/NOPE.BIN"); !errors.Is(err, ErrNotOnDisc) {
		t.Fatalf("missing file: %v", err)
	}
}

func TestListAndCopy(t *testing.T) {
	boot := []byte("0123456789")
	img := bytes.NewReader(syntheticISO(boot))
	files, err := List(img)
	if err != nil {
		t.Fatal(err)
	}
	var names []string
	for _, f := range files {
		names = append(names, f.Path)
	}
	want := []string{"PSP_GAME/SYSDIR/BOOT.BIN", "PSP_GAME/PARAM.SFO"}
	if len(names) != 2 || names[0] != want[0] || names[1] != want[1] {
		t.Fatalf("List = %v, want %v", names, want)
	}
	var out bytes.Buffer
	n, err := CopyFile(img, "PSP_GAME/SYSDIR/BOOT.BIN", &out)
	if err != nil || n != 10 || out.String() != "0123456789" {
		t.Fatalf("CopyFile: %d %q %v", n, out.String(), err)
	}
}

func TestNotAnISO(t *testing.T) {
	if _, err := ReadFileFrom(bytes.NewReader(make([]byte, 40*SectorSize)), "PSP_GAME/SYSDIR/BOOT.BIN"); !errors.Is(err, ErrNotISO) {
		t.Fatalf("zero image: %v", err)
	}
	if _, err := ReadFileFrom(bytes.NewReader([]byte("tiny")), "X"); err == nil {
		t.Fatal("a 4 byte file must not parse as an ISO")
	}
}
