// profile.go mirrors the profile handling of recomp/lean_gen.py and recomp/leangen/pipeline.py (load_profile, hooks, yield,
// noreturn, weak_args, import_arity, extra_inputs).
package leangen

import (
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
)

// Hook is one entry of the profile's "hooks".
type Hook struct {
	Name    string
	Enabled bool
}

// Arity is the (integer, float) argument register count of an import.
type Arity struct{ GPR, FPR int }

// Profile is the per-game generator profile (recomp/infected_profile.json).
type Profile struct {
	Noreturn    []uint32
	WeakArgs    int
	JalrWeakAll bool
	Yield       map[uint32]struct{}
	Hooks       map[uint32]Hook
	ImportArity map[uint32]Arity
	ExtraInputs map[uint32][]int
}

// pyInt ports Python's int(x, 0) for the forms a profile uses.
func pyInt(s string) (uint32, error) {
	s = strings.TrimSpace(s)
	v, err := strconv.ParseInt(s, 0, 64)
	if err != nil {
		return 0, fmt.Errorf("profile: bad number %q", s)
	}
	return uint32(v), nil
}

func asInt(v interface{}) int {
	switch x := v.(type) {
	case float64:
		return int(x)
	case bool:
		if x {
			return 1
		}
	}
	return 0
}

// ParseProfile reads a profile JSON document. An empty document is an empty profile.
func ParseProfile(data []byte) (*Profile, error) {
	p := &Profile{WeakArgs: 4, JalrWeakAll: true, Yield: map[uint32]struct{}{}, Hooks: map[uint32]Hook{},
		ImportArity: map[uint32]Arity{}, ExtraInputs: map[uint32][]int{}}
	if len(strings.TrimSpace(string(data))) == 0 {
		return p, nil
	}
	var raw map[string]interface{}
	if err := json.Unmarshal(data, &raw); err != nil {
		return nil, fmt.Errorf("profile: %v", err)
	}
	strList := func(key string) ([]uint32, error) {
		var out []uint32
		if l, ok := raw[key].([]interface{}); ok {
			for _, x := range l {
				s, ok := x.(string)
				if !ok {
					return nil, fmt.Errorf("profile: %s must be a list of strings", key)
				}
				v, err := pyInt(s)
				if err != nil {
					return nil, err
				}
				out = append(out, v)
			}
		}
		return out, nil
	}
	var err error
	if p.Noreturn, err = strList("noreturn"); err != nil {
		return nil, err
	}
	y, err := strList("yield")
	if err != nil {
		return nil, err
	}
	for _, a := range y {
		p.Yield[a] = struct{}{}
	}
	if v, ok := raw["weak_args"]; ok {
		p.WeakArgs = asInt(v)
	}
	if v, ok := raw["jalr_weak_all"]; ok {
		p.JalrWeakAll = asInt(v) != 0
	}
	if m, ok := raw["hooks"].(map[string]interface{}); ok {
		for k, v := range m {
			a, err := pyInt(k)
			if err != nil {
				return nil, err
			}
			h := Hook{Enabled: true}
			if hm, ok := v.(map[string]interface{}); ok {
				if n, ok := hm["name"].(string); ok {
					h.Name = n
				}
				if e, ok := hm["enabled"]; ok {
					h.Enabled = asInt(e) != 0
				}
			}
			p.Hooks[a] = h
		}
	}
	if m, ok := raw["import_arity"].(map[string]interface{}); ok {
		for k, v := range m {
			a, err := pyInt(k)
			if err != nil {
				return nil, err
			}
			ar := Arity{}
			if vm, ok := v.(map[string]interface{}); ok {
				ar.GPR = asInt(vm["gpr"])
				ar.FPR = asInt(vm["fpr"])
			}
			p.ImportArity[a] = ar
		}
	}
	if m, ok := raw["extra_inputs"].(map[string]interface{}); ok {
		for k, v := range m {
			a, err := pyInt(k)
			if err != nil {
				return nil, err
			}
			var regs []int
			if l, ok := v.([]interface{}); ok {
				for _, x := range l {
					regs = append(regs, asInt(x))
				}
			}
			p.ExtraInputs[a] = regs
		}
	}
	return p, nil
}
