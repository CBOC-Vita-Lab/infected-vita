// Package psp reads the player's PSP disc image: files out of an ISO9660 image and the relocated load image of its executable.
//
// iso.go mirrors tools/iso_get.py: copy one file out of an ISO9660 disc image (a UMD dump), using nothing but the standard library.
package psp

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"strings"
)

// SectorSize is the ISO9660 logical sector size.
const SectorSize = 2048

// File is one regular file found on the disc.
type File struct {
	Path string // path on the disc without the ";1" version suffix, e.g. "PSP_GAME/SYSDIR/BOOT.BIN"
	LBA  uint32
	Size uint32
}

// ErrNotISO is returned when the image is not an ISO9660 image.
var ErrNotISO = errors.New("not an ISO9660 image")

// ErrNotOnDisc is returned when the requested file is not on the disc.
var ErrNotOnDisc = errors.New("not on the disc")

func readAt(r io.ReaderAt, lba uint32, length uint32) ([]byte, error) {
	buf := make([]byte, length)
	n, err := r.ReadAt(buf, int64(lba)*SectorSize)
	if n == len(buf) {
		return buf, nil
	}
	if err == nil || err == io.EOF {
		// iso_get.py reads whatever is there; a short directory is treated as the data that exists
		return buf[:n], nil
	}
	return nil, err
}

type entry struct {
	name  string
	lba   uint32
	size  uint32
	isDir bool
}

// entries parses one directory extent (iso_get.py entries()).
func entries(data []byte) []entry {
	var out []entry
	i := 0
	for i < len(data) {
		n := int(data[i])
		if n == 0 {
			i = (i/SectorSize + 1) * SectorSize
			continue
		}
		if i+33 > len(data) {
			break
		}
		nameLen := int(data[i+32])
		if i+33+nameLen > len(data) || i+14 > len(data) {
			break
		}
		raw := data[i+33 : i+33+nameLen]
		name := string(latin1(raw))
		if k := strings.IndexByte(name, ';'); k >= 0 {
			name = name[:k]
		}
		lba := binary.LittleEndian.Uint32(data[i+2 : i+6])
		size := binary.LittleEndian.Uint32(data[i+10 : i+14])
		isDir := data[i+25]&2 != 0
		if !(len(raw) == 1 && (raw[0] == 0 || raw[0] == 1)) {
			out = append(out, entry{name, lba, size, isDir})
		}
		i += n
	}
	return out
}

func latin1(b []byte) []rune {
	r := make([]rune, len(b))
	for i, c := range b {
		r[i] = rune(c)
	}
	return r
}

func walk(r io.ReaderAt, lba, length uint32, prefix string, depth int, visit func(File) bool) (bool, error) {
	if depth > 32 {
		return false, nil // a directory loop in a corrupt image
	}
	data, err := readAt(r, lba, length)
	if err != nil {
		return false, err
	}
	for _, e := range entries(data) {
		path := prefix + e.name
		if e.isDir {
			stop, err := walk(r, e.lba, e.size, path+"/", depth+1, visit)
			if err != nil || stop {
				return stop, err
			}
		} else if visit(File{path, e.lba, e.size}) {
			return true, nil
		}
	}
	return false, nil
}

func root(r io.ReaderAt) (lba, length uint32, err error) {
	pvd, err := readAt(r, 16, SectorSize)
	if err != nil {
		return 0, 0, err
	}
	if len(pvd) < 190 || string(pvd[1:6]) != "CD001" {
		return 0, 0, ErrNotISO
	}
	rec := pvd[156 : 156+34]
	return binary.LittleEndian.Uint32(rec[2:6]), binary.LittleEndian.Uint32(rec[10:14]), nil
}

// List returns every file on the disc, in directory order.
func List(r io.ReaderAt) ([]File, error) {
	rl, rn, err := root(r)
	if err != nil {
		return nil, err
	}
	var files []File
	_, err = walk(r, rl, rn, "", 0, func(f File) bool { files = append(files, f); return false })
	return files, err
}

// Find looks a file up without regard to case and without the ISO's ";1" version suffix.
func Find(r io.ReaderAt, innerPath string) (File, error) {
	rl, rn, err := root(r)
	if err != nil {
		return File{}, err
	}
	want := strings.ToUpper(strings.Trim(innerPath, "/"))
	var found File
	ok := false
	_, err = walk(r, rl, rn, "", 0, func(f File) bool {
		if strings.ToUpper(f.Path) == want {
			found, ok = f, true
			return true
		}
		return false
	})
	if err != nil {
		return File{}, err
	}
	if !ok {
		return File{}, fmt.Errorf("%w: %s", ErrNotOnDisc, innerPath)
	}
	return found, nil
}

// CopyFile streams the named file to w and returns its size.
func CopyFile(r io.ReaderAt, innerPath string, w io.Writer) (int64, error) {
	f, err := Find(r, innerPath)
	if err != nil {
		return 0, err
	}
	sr := io.NewSectionReader(r, int64(f.LBA)*SectorSize, int64(f.Size))
	n, err := io.Copy(w, sr)
	if err == nil && n != int64(f.Size) {
		err = errors.New("short read")
	}
	return n, err
}

// ReadFileFrom returns the named file of an opened image.
func ReadFileFrom(r io.ReaderAt, innerPath string) ([]byte, error) {
	f, err := Find(r, innerPath)
	if err != nil {
		return nil, err
	}
	buf := make([]byte, f.Size)
	n, err := r.ReadAt(buf, int64(f.LBA)*SectorSize)
	if n != len(buf) {
		if err == nil || err == io.EOF {
			err = errors.New("short read")
		}
		return nil, err
	}
	return buf, nil
}

// ReadFile opens the ISO at isoPath and returns the file innerPath (for example "PSP_GAME/SYSDIR/BOOT.BIN").
func ReadFile(isoPath, innerPath string) ([]byte, error) {
	fh, err := os.Open(isoPath)
	if err != nil {
		return nil, err
	}
	defer fh.Close()
	return ReadFileFrom(fh, innerPath)
}
