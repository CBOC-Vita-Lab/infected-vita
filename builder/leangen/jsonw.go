// jsonw.go: a writer for the manifest in the exact layout of Python's json.dump(obj, fh, indent=0).
package leangen

import (
	"bufio"
	"strconv"
	"unicode/utf8"
)

type jnode interface{ write(w *bufio.Writer) }

type jint int64
type jstr string
type jbool bool
type jnull struct{}
type jarr []jnode
type jkv struct {
	k string
	v jnode
}
type jobj []jkv

func (j jint) write(w *bufio.Writer)  { w.WriteString(strconv.FormatInt(int64(j), 10)) }
func (j jstr) write(w *bufio.Writer)  { writeJSONString(w, string(j)) }
func (j jnull) write(w *bufio.Writer) { w.WriteString("null") }
func (j jbool) write(w *bufio.Writer) {
	if j {
		w.WriteString("true")
	} else {
		w.WriteString("false")
	}
}

func (a jarr) write(w *bufio.Writer) {
	if len(a) == 0 {
		w.WriteString("[]")
		return
	}
	w.WriteString("[\n")
	for i, x := range a {
		if i > 0 {
			w.WriteString(",\n")
		}
		x.write(w)
	}
	w.WriteString("\n]")
}

func (o jobj) write(w *bufio.Writer) {
	if len(o) == 0 {
		w.WriteString("{}")
		return
	}
	w.WriteString("{\n")
	for i, kv := range o {
		if i > 0 {
			w.WriteString(",\n")
		}
		writeJSONString(w, kv.k)
		w.WriteString(": ")
		kv.v.write(w)
	}
	w.WriteString("\n}")
}

func strArr(l []string) jarr {
	a := make(jarr, len(l))
	for i, s := range l {
		a[i] = jstr(s)
	}
	return a
}

const hexDigits = "0123456789abcdef"

// writeJSONString matches json.dumps with ensure_ascii=True.
func writeJSONString(w *bufio.Writer, s string) {
	w.WriteByte('"')
	for i := 0; i < len(s); {
		c := s[i]
		if c < utf8.RuneSelf {
			switch c {
			case '"':
				w.WriteString("\\\"")
			case '\\':
				w.WriteString("\\\\")
			case '\n':
				w.WriteString("\\n")
			case '\r':
				w.WriteString("\\r")
			case '\t':
				w.WriteString("\\t")
			case '\b':
				w.WriteString("\\b")
			case '\f':
				w.WriteString("\\f")
			default:
				if c < 0x20 || c == 0x7f && false {
					w.WriteString("\\u00")
					w.WriteByte(hexDigits[c>>4])
					w.WriteByte(hexDigits[c&15])
				} else {
					w.WriteByte(c)
				}
			}
			i++
			continue
		}
		r, size := utf8.DecodeRuneInString(s[i:])
		if r >= 0x10000 {
			r -= 0x10000
			hi, lo := 0xD800+(r>>10), 0xDC00+(r&0x3FF)
			writeU(w, hi)
			writeU(w, lo)
		} else {
			writeU(w, r)
		}
		i += size
	}
	w.WriteByte('"')
}

func writeU(w *bufio.Writer, r rune) {
	w.WriteString("\\u")
	w.WriteByte(hexDigits[(r>>12)&15])
	w.WriteByte(hexDigits[(r>>8)&15])
	w.WriteByte(hexDigits[(r>>4)&15])
	w.WriteByte(hexDigits[r&15])
}
