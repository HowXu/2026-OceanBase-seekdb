package proto

import (
	"math"
	"strconv"
	"unicode/utf8"

	"minivectordb/internal/engine"
	"minivectordb/internal/types"
)

// Encoder writes one JSONL response per request (see STDIO_PROTOCOL.md):
// {"id":..,"ok":true,"columns":[..],"rows":[[..]]} for queries,
// {"id":..,"ok":true,"affected_rows":N} for writes, {"id":..,"ok":true} for
// other statements and {"id":..,"ok":false,"error":{"code":..,"message":..}}
// for failures. Each response is a single line ending in '\n'.
type Encoder struct {
	buf []byte
}

// Ready is the startup handshake line.
const Ready = "{\"type\":\"ready\"}\n"

// jsonString appends s as a JSON string. Valid non-ASCII UTF-8 is written as
// is; invalid bytes (e.g. a message truncated mid-rune) become U+FFFD so the
// line always decodes as UTF-8.
func jsonString(b []byte, s string) []byte {
	const hex = "0123456789abcdef"
	b = append(b, '"')
	for i := 0; i < len(s); {
		c := s[i]
		if c >= utf8.RuneSelf {
			r, n := utf8.DecodeRuneInString(s[i:])
			if r == utf8.RuneError && n == 1 {
				b = append(b, "\uFFFD"...)
			} else {
				b = append(b, s[i:i+n]...)
			}
			i += n
			continue
		}
		i++
		switch {
		case c == '"' || c == '\\':
			b = append(b, '\\', c)
		case c == '\n':
			b = append(b, '\\', 'n')
		case c == '\r':
			b = append(b, '\\', 'r')
		case c == '\t':
			b = append(b, '\\', 't')
		case c < 0x20:
			b = append(b, '\\', 'u', '0', '0', hex[c>>4], hex[c&15])
		default:
			b = append(b, c)
		}
	}
	return append(b, '"')
}

func appendFloat(b []byte, f float64) []byte {
	if math.IsNaN(f) || math.IsInf(f, 0) {
		return append(b, "null"...)
	}
	if f == math.Trunc(f) && math.Abs(f) < 1e15 {
		b = strconv.AppendFloat(b, f, 'f', -1, 64)
		return append(b, ".0"...)
	}
	return strconv.AppendFloat(b, f, 'g', 17, 64)
}

func appendValue(b []byte, v types.Value) []byte {
	switch v.K {
	case types.KInt:
		return strconv.AppendInt(b, v.I, 10)
	case types.KFloat:
		return appendFloat(b, v.F)
	case types.KVec:
		b = append(b, '[')
		for i, f := range v.V {
			if i > 0 {
				b = append(b, ',')
			}
			b = appendFloat32(b, f)
		}
		return append(b, ']')
	}
	return jsonString(b, v.S)
}

// appendFloat32 writes the shortest decimal that round-trips to f.
func appendFloat32(b []byte, f float32) []byte {
	if math.IsNaN(float64(f)) || math.IsInf(float64(f), 0) {
		return append(b, "null"...)
	}
	return strconv.AppendFloat(b, float64(f), 'g', -1, 32)
}

// head starts a response object with the request id echoed verbatim.
func head(b []byte, id []byte, ok bool) []byte {
	b = append(b, `{"id":`...)
	if len(id) == 0 {
		b = append(b, "null"...)
	} else {
		b = append(b, id...)
	}
	if ok {
		return append(b, `,"ok":true`...)
	}
	return append(b, `,"ok":false`...)
}

// OK is a success response without result fields.
func (e *Encoder) OK(id []byte) []byte {
	b := head(e.buf[:0], id, true)
	b = append(b, "}\n"...)
	e.buf = b
	return b
}

func (e *Encoder) Result(id []byte, r *engine.Result) []byte {
	b := head(e.buf[:0], id, true)
	if r.IsQuery {
		b = append(b, `,"columns":[`...)
		for i, c := range r.Columns {
			if i > 0 {
				b = append(b, ',')
			}
			b = jsonString(b, c)
		}
		b = append(b, `],"rows":[`...)
		for i, row := range r.Rows {
			if i > 0 {
				b = append(b, ',')
			}
			b = append(b, '[')
			for j, v := range row {
				if j > 0 {
					b = append(b, ',')
				}
				b = appendValue(b, v)
			}
			b = append(b, ']')
		}
		b = append(b, "]}\n"...)
	} else {
		b = append(b, `,"affected_rows":`...)
		b = strconv.AppendInt(b, r.Affected, 10)
		b = append(b, "}\n"...)
	}
	e.buf = b
	return b
}

// Error writes a failure response. code is the semantic error name, e.g.
// duplicate_key; the MySQL error number and SQLSTATE go into the message.
func (e *Encoder) Error(id []byte, code, message string) []byte {
	if code == "" {
		code = "internal_error"
	}
	b := head(e.buf[:0], id, false)
	b = append(b, `,"error":{"code":`...)
	b = jsonString(b, code)
	b = append(b, `,"message":`...)
	b = jsonString(b, message)
	b = append(b, "}}\n"...)
	e.buf = b
	return b
}

// SQLError writes a failure response for a SQL error.
func (e *Encoder) SQLError(id []byte, err *types.SQLError) []byte {
	return e.Error(id, err.Name, err.Error())
}
