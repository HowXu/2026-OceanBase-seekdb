// Package types holds runtime values, row encoding and SQL error definitions.
package types

import (
	"encoding/binary"
	"math"
	"math/bits"
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

// trimSpace is strings.TrimSpace with an inlined fast path.
//
// The vector parser trims every element, and the elements of a client vector
// carry no spaces at all, so the call and the scan are pure overhead. The
// fast path fires only when both ends are plain ASCII, which is exactly the
// case in which strings.TrimSpace returns the string unchanged: every ASCII
// whitespace character is at most 0x20, and a byte below 0x80 means the rune
// at that end is ASCII, so it cannot be one of the Unicode spaces either.
// Everything else is left to strings.TrimSpace.
func trimSpace(s string) string {
	if len(s) == 0 {
		return s
	}
	f, l := s[0], s[len(s)-1]
	if f > ' ' && f < 0x80 && l > ' ' && l < 0x80 {
		return s
	}
	return strings.TrimSpace(s)
}

// ParseVector parses '[a, b, ...]' into float32 values, rejecting NaN, Inf,
// overflow and malformed numbers.
func ParseVector(s string) ([]float32, *SQLError) {
	t := trimSpace(s)
	if len(t) < 2 || t[0] != '[' || t[len(t)-1] != ']' {
		return nil, ErrVectorFormat(s)
	}
	body := trimSpace(t[1 : len(t)-1])
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
			if len(trimSpace(body)) == 0 {
				return nil, ErrVectorFormat(s)
			}
		}
		part = trimSpace(part)
		f, ok := parseFloatFast(part)
		if !ok {
			// Slow path: anything the single-pass scanner does not handle
			// exactly. This is also the path that decides whether the literal
			// is well formed at all.
			if !validNumber(part) {
				return nil, ErrVectorFormat(s)
			}
			f64, err := strconv.ParseFloat(part, 64)
			if err != nil && !math.IsInf(f64, 0) {
				return nil, ErrVectorFormat(s)
			}
			f = f64
		}
		f32, ok := ToFloat32(f)
		if !ok {
			return nil, ErrVectorValue(part)
		}
		out = append(out, f32)
	}
	return out, nil
}

// pow10 holds the powers of ten that are exactly representable in float64.
// 10^23 is the first one that is not, because 5^23 no longer fits in the
// 53-bit significand.
var pow10 = [...]float64{
	1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
	1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
}

// maxFastMant is the largest mantissa the cheap path of decimalToFloat
// accepts. It is chosen so that mant*10+9 still fits in the 53-bit
// significand of float64, which keeps float64(mant) exact.
const maxFastMant = 900719925474098

// maxFastDigits is the number of mantissa digits parseFloatFast can hold
// without overflowing its uint64 accumulator.
const maxFastDigits = 19

// pow5 holds the powers of five that fit in a uint64. 5^28 is the first one
// that does not.
var pow5 = [...]uint64{
	1, 5, 25, 125, 625, 3125, 15625, 78125, 390625, 1953125,
	9765625, 48828125, 244140625, 1220703125, 6103515625, 30517578125,
	152587890625, 762939453125, 3814697265625, 19073486328125,
	95367431640625, 476837158203125, 2384185791015625, 11920928955078125,
	59604644775390625, 298023223876953125, 1490116119384765625,
	7450580596923828125,
}

// smallestNormal is the smallest positive normal float64, 2^-1022. Values
// below it are subnormal and are left to strconv.
const smallestNormal = 2.2250738585072014e-308

// parseFloatFast parses a plain decimal literal in one pass and returns the
// very same float64 that strconv.ParseFloat would return.
//
// It accepts the grammar of validNumber with an ASCII mantissa of at most 19
// digits and a decimal exponent of at most eight digits, and every value it
// accepts is converted exactly. Two paths do the work: the cheap path takes a
// mantissa that is exactly representable in float64 together with a power of
// ten that is itself exact (|k| <= 22), where a single IEEE multiplication or
// division is correctly rounded and therefore bit-identical to strconv; the
// exact path takes the longer mantissas the client sends and rounds them with
// one 128/64 division and the round-to-nearest-even rule strconv uses.
//
// It reports ok=false for every input it cannot convert exactly: a malformed
// literal, a mantissa of more than 19 digits, a subnormal or out of range
// result, and anything the two paths above decline. Those inputs are handled
// by strconv.ParseFloat.
func parseFloatFast(s string) (float64, bool) {
	n := len(s)
	i := 0
	neg := false
	if i < n && (s[i] == '+' || s[i] == '-') {
		neg = s[i] == '-'
		i++
	}
	var mant uint64
	digits := 0     // total number of mantissa digits
	fracDigits := 0 // number of digits after the decimal point
	// The integer part. Eight digits at a time where possible: the block
	// conversion is a handful of instructions for eight characters.
	for i+8 <= n {
		v := le64(s, i)
		if !eightDigits(v) {
			break
		}
		mant = mant*1e8 + eightDigitsValue(v)
		digits += 8
		i += 8
	}
	for i < n {
		c := s[i] - '0'
		if c > 9 {
			break
		}
		mant = mant*10 + uint64(c)
		digits++
		i++
	}
	if i < n && s[i] == '.' {
		i++
		for i+8 <= n {
			v := le64(s, i)
			if !eightDigits(v) {
				break
			}
			mant = mant*1e8 + eightDigitsValue(v)
			digits += 8
			fracDigits += 8
			i += 8
		}
		for i < n {
			c := s[i] - '0'
			if c > 9 {
				break
			}
			mant = mant*10 + uint64(c)
			digits++
			fracDigits++
			i++
		}
	}
	if digits == 0 {
		return 0, false
	}
	exp := 0
	if i < n && (s[i] == 'e' || s[i] == 'E') {
		i++
		negExp := false
		if i < n && (s[i] == '+' || s[i] == '-') {
			negExp = s[i] == '-'
			i++
		}
		expDigits := 0
		for i < n && s[i] >= '0' && s[i] <= '9' {
			if expDigits >= 9 {
				// The exponent is far outside any range the conversion
				// below can use. Leave it to strconv.
				return 0, false
			}
			exp = exp*10 + int(s[i]-'0')
			expDigits++
			i++
		}
		if expDigits == 0 {
			return 0, false
		}
		if negExp {
			exp = -exp
		}
	}
	if i != n {
		return 0, false
	}
	if digits > maxFastDigits {
		// The accumulator may have wrapped; the value is not exact anyway.
		return 0, false
	}
	k := exp - fracDigits
	f, ok := decimalToFloat(mant, k)
	if !ok {
		return 0, false
	}
	if neg {
		f = -f
	}
	return f, true
}

// decimalToFloat converts the decimal value mant*10^k to the float64 that is
// nearest to it.
//
// Two paths are used. The cheap one applies when the mantissa is exactly
// representable in float64 and the power of ten is exact as well: a single
// IEEE multiplication or division then produces the correctly rounded result.
// The exact path handles the longer mantissas the workload uses: it divides
// the scaled numerator by the power of five with one 128/64 division and
// rounds the quotient to a 53 bit significand. Both apply the rounding rule
// strconv applies, so both give the same bits.
//
// It reports ok=false when the value cannot be carried out exactly.
func decimalToFloat(mant uint64, k int) (float64, bool) {
	if mant == 0 {
		return 0, true
	}
	if mant <= maxFastMant*10+9 && k >= -22 && k <= 22 {
		f := float64(mant)
		switch {
		case k == 0:
		case k > 0:
			f *= pow10[k]
		default:
			f /= pow10[-k]
		}
		return f, true
	}
	return decimalExact(mant, k)
}

// decimalExact converts mant*10^k to the nearest float64 with a single
// 128/64 division.
//
// The decimal value is mant * 2^k / 5^k. The numerator is scaled so that the
// quotient carries 54 or 55 bits, which leaves room for the bits that decide
// the rounding, and the quotient is then rounded to a 53 bit significand with
// the round-to-nearest-even rule that strconv uses. The result is therefore
// bit-identical to strconv.ParseFloat.
//
// It reports ok=false for every input it cannot prove to be exact: a power
// of five that does not fit in a uint64, a numerator that cannot be scaled,
// or a result outside the normal float64 range.
func decimalExact(mant uint64, k int) (float64, bool) {
	if k > 0 {
		// mant * 10^k = (mant * 5^k) * 2^k. Scaling by a power of two is
		// exact, so rounding the integer product once rounds the whole value.
		if k >= len(pow5) {
			return 0, false
		}
		p := mant
		for i := 0; i < k; i++ {
			hi, lo := bits.Mul64(p, 5)
			if hi != 0 {
				return 0, false
			}
			p = lo
		}
		f := math.Ldexp(float64(p), k)
		if f == 0 || math.IsInf(f, 0) || math.Abs(f) < smallestNormal {
			return 0, false
		}
		return f, true
	}
	kk := -k
	if kk >= len(pow5) {
		return 0, false
	}
	d := pow5[kk]
	// Scale the numerator so that the quotient carries 54 or 55 bits: 53 bits
	// of significand plus the one or two bits that decide the rounding. The
	// shift below is always derived from the length of the quotient, never
	// assumed, because the quotient length depends on the mantissa.
	s := 54 + bits.Len64(d) - bits.Len64(mant)
	if s < 0 {
		return 0, false
	}
	var hi, lo uint64
	if s >= 64 {
		hi = mant << (s - 64)
		lo = 0
	} else {
		hi = mant >> (64 - s)
		lo = mant << s
	}
	if hi >= d {
		return 0, false
	}
	q, r := bits.Div64(hi, lo, d)
	shift := bits.Len64(q) - 53
	if shift < 0 {
		return 0, false
	}
	var sig uint64
	var up bool
	if shift == 0 {
		// The scaling above keeps the quotient at 54 or 55 bits, so this
		// branch is not reached today. It is kept because the comparison is
		// the one that decides the rounding when no bit is dropped: compare
		// the remainder with a half.
		switch {
		case 2*r > d:
			up = true
		case 2*r == d:
			up = q&1 == 1
		}
		sig = q
	} else {
		drop := q & (1<<shift - 1)
		half := uint64(1) << (shift - 1)
		switch {
		case drop > half:
			up = true
		case drop == half:
			if r != 0 {
				up = true
			} else {
				// Exactly half: round to even.
				up = q>>shift&1 == 1
			}
		}
		sig = q >> shift
	}
	if up {
		sig++
	}
	exp := -(s + kk) + shift
	if sig == 1<<53 {
		sig >>= 1
		exp++
	}
	e := exp + 52
	if e < -1022 || e > 1023 {
		return 0, false
	}
	return math.Float64frombits(uint64(e+1023)<<52 | (sig & (1<<52 - 1))), true
}

// le64 reads the eight bytes of s starting at i as a little-endian word.
func le64(s string, i int) uint64 {
	return uint64(s[i]) | uint64(s[i+1])<<8 | uint64(s[i+2])<<16 | uint64(s[i+3])<<24 |
		uint64(s[i+4])<<32 | uint64(s[i+5])<<40 | uint64(s[i+6])<<48 | uint64(s[i+7])<<56
}

// eightDigits reports whether the eight bytes of v are all ASCII digits.
func eightDigits(v uint64) bool {
	// Every byte must be in 0x30..0x3f, and adding six must keep it in
	// 0x30..0x3f as well, which leaves exactly '0'..'9'.
	return v&0xf0f0f0f0f0f0f0f0 == 0x3030303030303030 &&
		(v+0x0606060606060606)&0xf0f0f0f0f0f0f0f0 == 0x3030303030303030
}

// eightDigitsValue returns the number spelled by eight ASCII digits. The
// first character of the block is the most significant digit.
func eightDigitsValue(v uint64) uint64 {
	v -= 0x3030303030303030
	v = (v*10 + (v >> 8)) & 0x00ff00ff00ff00ff
	v = (v*100 + (v >> 16)) & 0x0000ffff0000ffff
	v = (v*10000 + (v >> 32)) & 0x00000000ffffffff
	return v
}

// maxFloat32 is the largest finite float32. A float64 that is NaN or
// infinite, and one that is too large for a float32, all turn into a float32
// that is either NaN or outside this range.
const maxFloat32 = 3.4028234663852886e+38

// ToFloat32 converts a finite float64 to float32, failing on NaN, Inf or overflow.
func ToFloat32(f float64) (float32, bool) {
	r := float32(f)
	if !(r >= -maxFloat32 && r <= maxFloat32) {
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
