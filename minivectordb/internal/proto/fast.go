package proto

import (
	"encoding/json"
	"unicode/utf8"
)

// parseFast decodes the common request shape without reflection: a flat
// object whose values are strings without escapes or plain numbers, for
// example {"id":7,"op":"execute","sql":"SELECT ...","search_mode":"exact"}.
// Anything else (escapes, nesting, duplicate or unknown structure) returns
// ok=false and the caller falls back to encoding/json, so the accepted
// language is a strict subset of what encoding/json accepts with identical
// results.
func parseFast(b []byte) (*Request, bool) {
	i, n := 0, len(b)
	ws := func() {
		for i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n') {
			i++
		}
	}
	// str reads a string with no escapes and no control characters.
	str := func() (string, bool) {
		if i >= n || b[i] != '"' {
			return "", false
		}
		j := i + 1
		for j < n {
			c := b[j]
			if c == '"' {
				if !utf8.Valid(b[i+1 : j]) {
					// encoding/json would replace invalid bytes with U+FFFD.
					return "", false
				}
				s := string(b[i+1 : j])
				i = j + 1
				return s, true
			}
			if c == '\\' || c < 0x20 {
				return "", false
			}
			j++
		}
		return "", false
	}
	ws()
	if i >= n || b[i] != '{' {
		return nil, false
	}
	i++
	var r Request
	// The only keys the fast path accepts are op, sql, search_mode and id,
	// so four flags replace a map that had to be allocated per request.
	var seenOp, seenSQL, seenMode, seenID bool
	for {
		ws()
		if i < n && b[i] == '}' && !seenOp && !seenSQL && !seenMode && !seenID {
			i++
			break
		}
		k, ok := str()
		if !ok {
			return nil, false
		}
		ws()
		if i >= n || b[i] != ':' {
			return nil, false
		}
		i++
		ws()
		switch k {
		case "op":
			if seenOp {
				return nil, false
			}
			seenOp = true
			v, ok := str()
			if !ok {
				return nil, false
			}
			r.Op = v
		case "sql":
			if seenSQL {
				return nil, false
			}
			seenSQL = true
			v, ok := str()
			if !ok {
				return nil, false
			}
			r.SQL = &v
		case "search_mode":
			if seenMode {
				return nil, false
			}
			seenMode = true
			v, ok := str()
			if !ok {
				return nil, false
			}
			r.SearchMode = v
		case "id":
			if seenID {
				return nil, false
			}
			seenID = true
			start := i
			if i < n && b[i] == '"' {
				if _, ok := str(); !ok {
					return nil, false
				}
			} else {
				if i < n && b[i] == '-' {
					i++
				}
				d := 0
				for i < n && b[i] >= '0' && b[i] <= '9' {
					i++
					d++
				}
				if d == 0 || (d > 1 && b[start] == '0') || (d > 1 && b[start] == '-' && b[start+1] == '0') {
					return nil, false
				}
			}
			r.ID = json.RawMessage(append([]byte(nil), b[start:i]...))
		default:
			return nil, false
		}
		ws()
		if i < n && b[i] == ',' {
			i++
			continue
		}
		if i < n && b[i] == '}' {
			i++
			break
		}
		return nil, false
	}
	ws()
	if i != n || r.ID == nil {
		return nil, false
	}
	return &r, true
}
