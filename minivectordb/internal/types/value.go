// Package types holds runtime values, row encoding and SQL error definitions.
package types

import (
	"encoding/binary"
	"math"
	"strconv"
	"strings"
	"unicode/utf8"
)

type Kind uint8

const (
	KInt Kind = iota + 1
	KFloat
	KStr
	KVec
)

type Value struct {
	K Kind
	I int64
	F float64
	S string
	V []float32
}

func Int(v int64) Value     { return Value{K: KInt, I: v} }
func Float(v float64) Value { return Value{K: KFloat, F: v} }
func Str(v string) Value    { return Value{K: KStr, S: v} }
func Vec(v []float32) Value { return Value{K: KVec, V: v} }
func (v Value) IsNum() bool { return v.K == KInt || v.K == KFloat }
func (v Value) Num() float64 {
	if v.K == KInt {
		return float64(v.I)
	}
	return v.F
}

// ParseVector parses '[a, b, ...]' into float32 values, rejecting NaN, Inf,
// overflow and malformed numbers.
func ParseVector(s string) ([]float32, *SQLError) {
	t := strings.TrimSpace(s)
	if len(t) < 2 || t[0] != '[' || t[len(t)-1] != ']' {
		return nil, ErrVectorFormat(s)
	}
	body := strings.TrimSpace(t[1 : len(t)-1])
	if body == "" {
		return []float32{}, nil
	}
	out := make([]float32, 0, strings.Count(body, ",")+1)
	for len(body) > 0 {
		i := strings.IndexByte(body, ',')
		var part string
		if i < 0 {
			part, body = body, ""
		} else {
			part, body = body[:i], body[i+1:]
			if strings.TrimSpace(body) == "" {
				return nil, ErrVectorFormat(s)
			}
		}
		part = strings.TrimSpace(part)
		if !validNumber(part) {
			return nil, ErrVectorFormat(s)
		}
		f, err := strconv.ParseFloat(part, 64)
		if err != nil && !math.IsInf(f, 0) {
			return nil, ErrVectorFormat(s)
		}
		f32, ok := ToFloat32(f)
		if !ok {
			return nil, ErrVectorValue(part)
		}
		out = append(out, f32)
	}
	return out, nil
}

// ToFloat32 converts a finite float64 to float32, failing on NaN, Inf or overflow.
func ToFloat32(f float64) (float32, bool) {
	if math.IsNaN(f) || math.IsInf(f, 0) {
		return 0, false
	}
	r := float32(f)
	if math.IsInf(float64(r), 0) {
		return 0, false
	}
	return r, true
}

// validNumber accepts [+-]digits[.digits][e[+-]digits] and [+-].digits forms only.
func validNumber(s string) bool {
	i, n := 0, len(s)
	if i < n && (s[i] == '+' || s[i] == '-') {
		i++
	}
	digits := 0
	for i < n && s[i] >= '0' && s[i] <= '9' {
		i++
		digits++
	}
	if i < n && s[i] == '.' {
		i++
		for i < n && s[i] >= '0' && s[i] <= '9' {
			i++
			digits++
		}
	}
	if digits == 0 {
		return false
	}
	if i < n && (s[i] == 'e' || s[i] == 'E') {
		i++
		if i < n && (s[i] == '+' || s[i] == '-') {
			i++
		}
		ed := 0
		for i < n && s[i] >= '0' && s[i] <= '9' {
			i++
			ed++
		}
		if ed == 0 {
			return false
		}
	}
	return i == n
}

// FormatVector renders a vector the way it is accepted on input.
func FormatVector(v []float32) string {
	b := make([]byte, 0, len(v)*8+2)
	b = append(b, '[')
	for i, f := range v {
		if i > 0 {
			b = append(b, ',')
		}
		b = strconv.AppendFloat(b, float64(f), 'g', -1, 32)
	}
	return string(append(b, ']'))
}

func CharLen(s string) int { return utf8.RuneCountInString(s) }

// Column is the storage view of a table column.
type Column struct {
	Name  string
	Type  uint8 // 1 BIGINT, 2 VARCHAR, 3 VECTOR (matches sqlparse.ColType)
	Param int
}

const (
	TBigint  = 1
	TVarchar = 2
	TVector  = 3
)

// EncodeRow serialises values in column order: BIGINT as 8 bytes LE, VARCHAR as
// uvarint length + bytes, VECTOR as dim float32 LE values.
func EncodeRow(dst []byte, cols []Column, row []Value) []byte {
	for i, c := range cols {
		v := row[i]
		switch c.Type {
		case TBigint:
			dst = binary.LittleEndian.AppendUint64(dst, uint64(v.I))
		case TVarchar:
			dst = binary.AppendUvarint(dst, uint64(len(v.S)))
			dst = append(dst, v.S...)
		case TVector:
			for _, f := range v.V {
				dst = binary.LittleEndian.AppendUint32(dst, math.Float32bits(f))
			}
		}
	}
	return dst
}

// DecodeRow decodes all columns.
func DecodeRow(b []byte, cols []Column) []Value {
	out := make([]Value, len(cols))
	p := 0
	for i, c := range cols {
		out[i], p = decodeAt(b, p, c, true)
	}
	return out
}

// ColOffset returns the byte range of column idx within an encoded row.
func ColOffset(b []byte, cols []Column, idx int) (int, int) {
	p := 0
	for i := 0; i < idx; i++ {
		p = skip(b, p, cols[i])
	}
	return p, skip(b, p, cols[idx])
}

func skip(b []byte, p int, c Column) int {
	switch c.Type {
	case TBigint:
		return p + 8
	case TVarchar:
		l, n := binary.Uvarint(b[p:])
		return p + n + int(l)
	default:
		return p + 4*c.Param
	}
}

// DecodeCol decodes a single column.
func DecodeCol(b []byte, cols []Column, idx int) Value {
	p := 0
	for i := 0; i < idx; i++ {
		p = skip(b, p, cols[i])
	}
	v, _ := decodeAt(b, p, cols[idx], true)
	return v
}

func decodeAt(b []byte, p int, c Column, copyVec bool) (Value, int) {
	switch c.Type {
	case TBigint:
		return Int(int64(binary.LittleEndian.Uint64(b[p:]))), p + 8
	case TVarchar:
		l, n := binary.Uvarint(b[p:])
		s := p + n
		return Str(string(b[s : s+int(l)])), s + int(l)
	default:
		v := make([]float32, c.Param)
		DecodeVecInto(v, b[p:])
		return Vec(v), p + 4*c.Param
	}
}

// DecodeVecInto fills dst from little-endian float32 bytes.
func DecodeVecInto(dst []float32, b []byte) {
	_ = b[4*len(dst)-1]
	for i := range dst {
		dst[i] = math.Float32frombits(binary.LittleEndian.Uint32(b[4*i:]))
	}
}
