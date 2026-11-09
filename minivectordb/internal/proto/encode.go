package proto

import (
	"math"
	"strconv"

	"minivectordb/internal/engine"
	"minivectordb/internal/types"
)

// Encoder writes one JSON object per statement, using the same field names as
// the .result assertions: columns/rows for queries, affected_rows for writes,
// error for failures. Each object is a single line.
type Encoder struct {
	buf []byte
}

// jsonString appends s as a JSON string. Non-ASCII UTF-8 is written as is.
func jsonString(b []byte, s string) []byte {
	const hex = "0123456789abcdef"
	b = append(b, '"')
	for i := 0; i < len(s); i++ {
		c := s[i]
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
		return jsonString(b, types.FormatVector(v.V))
	}
	return jsonString(b, v.S)
}

func (e *Encoder) Result(r *engine.Result) []byte {
	b := e.buf[:0]
	if r.IsQuery {
		b = append(b, `{"columns":[`...)
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
		b = append(b, `{"affected_rows":`...)
		b = strconv.AppendInt(b, r.Affected, 10)
		b = append(b, "}\n"...)
	}
	e.buf = b
	return b
}

func (e *Encoder) Error(err *types.SQLError) []byte {
	b := e.buf[:0]
	b = append(b, `{"error":`...)
	b = jsonString(b, err.Name)
	b = append(b, `,"code":`...)
	b = strconv.AppendInt(b, int64(err.Code), 10)
	b = append(b, `,"sqlstate":`...)
	b = jsonString(b, err.State)
	b = append(b, `,"message":`...)
	b = jsonString(b, err.Msg)
	b = append(b, "}\n"...)
	e.buf = b
	return b
}
