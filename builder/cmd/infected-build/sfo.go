package main

import "encoding/binary"

// parseSFO reads the key/value pairs of a PSP PARAM.SFO (strings and integers as text), or nil when it is not one.
func parseSFO(b []byte) map[string]string {
	if len(b) < 20 || string(b[:4]) != "\x00PSF" {
		return nil
	}
	le := binary.LittleEndian
	keyTab, dataTab, n := int(le.Uint32(b[8:])), int(le.Uint32(b[12:])), int(le.Uint32(b[16:]))
	out := map[string]string{}
	for i := 0; i < n; i++ {
		e := 20 + 16*i
		if e+16 > len(b) {
			break
		}
		keyOff, format, length, dataOff := keyTab+int(le.Uint16(b[e:])), le.Uint16(b[e+2:]), int(le.Uint32(b[e+4:])), dataTab+int(le.Uint32(b[e+12:]))
		if keyOff >= len(b) || dataOff+length > len(b) || dataOff < 0 {
			continue
		}
		end := keyOff
		for end < len(b) && b[end] != 0 {
			end++
		}
		key := string(b[keyOff:end])
		val := b[dataOff : dataOff+length]
		switch format {
		case 0x0004, 0x0204: // UTF-8 text, with (0x0004) or without padding
			for len(val) > 0 && val[len(val)-1] == 0 {
				val = val[:len(val)-1]
			}
			out[key] = string(val)
		}
	}
	return out
}
