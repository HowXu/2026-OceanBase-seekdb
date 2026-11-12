package types

import (
	"fmt"
	"math"
	"math/rand"
	"strconv"
	"strings"
	"testing"
)

// parseVectorReference is the original implementation, kept verbatim as the
// behavioural oracle for ParseVector. Any change to ParseVector must keep the
// two in lockstep: same accept/reject decision, same error, and bit-identical
// float32 results.
func parseVectorReference(s string) ([]float32, *SQLError) {
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

// sameResult reports whether two outcomes agree on acceptance, error identity
// and every float32 bit pattern.
func sameResult(t *testing.T, in string, got []float32, gotErr *SQLError, want []float32, wantErr *SQLError) {
	t.Helper()
	if (gotErr == nil) != (wantErr == nil) {
		t.Fatalf("%q: error mismatch: got %v, want %v", in, gotErr, wantErr)
	}
	if gotErr != nil {
		if gotErr.Name != wantErr.Name || gotErr.Code != wantErr.Code || gotErr.State != wantErr.State {
			t.Fatalf("%q: error detail mismatch: got %+v, want %+v", in, gotErr, wantErr)
		}
		if gotErr.Msg != wantErr.Msg {
			t.Fatalf("%q: error message mismatch:\n got %q\nwant %q", in, gotErr.Msg, wantErr.Msg)
		}
		return
	}
	if len(got) != len(want) {
		t.Fatalf("%q: length mismatch: got %d, want %d", in, len(got), len(want))
	}
	for i := range got {
		if math.Float32bits(got[i]) != math.Float32bits(want[i]) {
			t.Fatalf("%q: element %d differs: got %v (%08x), want %v (%08x)",
				in, i, got[i], math.Float32bits(got[i]), want[i], math.Float32bits(want[i]))
		}
	}
}

// checkAgainstReference runs both implementations on the same input.
func checkAgainstReference(t *testing.T, in string) {
	t.Helper()
	got, gotErr := ParseVector(in)
	want, wantErr := parseVectorReference(in)
	sameResult(t, in, got, gotErr, want, wantErr)
}

// TestParseVectorMatchesReference compares ParseVector with the original
// implementation over generated literals, edge cases and malformed input.
func TestParseVectorMatchesReference(t *testing.T) {
	rng := rand.New(rand.NewSource(11))

	// 1. Plain decimal literals in several shapes.
	shapes := []string{
		"%v", "%e", "%g", "%.17g", "%.9f", "%.6f", "%.20f", "%.1f", "%.0f",
		"%v", "%v", "%v", "%v",
	}
	for _, shape := range shapes {
		for i := 0; i < 400; i++ {
			var f float64
			switch i % 4 {
			case 0:
				f = rng.Float64()
			case 1:
				f = rng.Float64() * 1e-6
			case 2:
				f = rng.Float64() * 1e6
			default:
				f = float64(rng.Intn(2001) - 1000)
			}
			s := fmt.Sprintf(shape, f)
			checkAgainstReference(t, "["+s+"]")
			checkAgainstReference(t, "["+s+", "+s+"]")
		}
	}

	// 2. float32 round-trip values (what the benchmark and the judge send).
	for i := 0; i < 3000; i++ {
		f := float32(rng.NormFloat64())
		s := fmt.Sprintf("%v", f)
		checkAgainstReference(t, "["+s+"]")
	}

	// 3. Explicitly constructed literals with many digits and exponents.
	manual := []string{
		"0", "-0", "+0", "0.0", "-0.0", "1", "-1", "+1",
		"1.", "-1.", ".5", "-.5", "+.5", "0.5", "00.50",
		"1e0", "1E0", "1e+5", "1e-5", "1e+22", "1e-22", "1e23", "1e-23",
		"1e308", "1e309", "1e-308", "1e-309", "1e-323", "1e-324", "1e-400",
		"1.7976931348623157e308", "1.7976931348623159e308",
		"3.4028234663852886e38", "3.4028235e38", "3.4028236e38",
		"1.1754943508222875e-38", "1.1754944e-38",
		"9007199254740991", "9007199254740993", "9007199254740992",
		"0.1", "0.2", "0.3", "1.1", "2.2", "3.3", "12345678901234567890",
		"0.00000000000000000001", "1e-20", "1e20",
		"1000000000000000000000", "1e21", "1e22", "1e-22",
		"123456789012345678901234567890e-30",
		"0.000000000000000000000000000001",
		"4.9406564584124654e-324", "2.2250738585072014e-308",
		"5e-324", "1e-323",
	}
	for _, m := range manual {
		checkAgainstReference(t, "["+m+"]")
		checkAgainstReference(t, "["+m+",1,"+m+"]")
	}

	// 4. Whitespace and formatting variants around the literals.
	spaces := []string{
		"[ 1 ]", "[1]", "  [1]  ", "[1,2]", "[1, 2]", "[ 1 , 2 ]", "[1 ,2]",
		"[\t1\n,\r2\t]", "[]", " [ ] ", "[ ]", "[,]", "[1,]", "[,1]", "[1,,2]",
		"[1 2]", "[1,2 3]", "[+1,-2]", "[1e5,2E-5]",
	}
	for _, s := range spaces {
		checkAgainstReference(t, s)
	}

	// 5. Malformed and rejected values.
	bad := []string{
		"", "[", "]", "1", "abc", "[abc]", "[1,abc]", "[nan]", "[NaN]", "[inf]",
		"[Inf]", "[Infinity]", "[+Inf]", "[-Inf]", "[infinity]", "[1,2", "[1,2]]",
		"[[1]]", "[1e]", "[1e+]", "[1e-]", "[1.2.3]", "[1_0]", "[0x1]", "[1.0f]",
		"[--1]", "[1-]", "[1+2]", "[1e1e1]", "[.]", "[1.]", "[+]", "[-]",
		"[1e1000000000000000000000]", "[1e-1000000000000000000000]",
		"[--]", "[1,,]", "[1, ,2]",
	}
	for _, s := range bad {
		checkAgainstReference(t, s)
	}

	// 6. Long vectors: mixed values and a single malformed element at the end.
	for i := 0; i < 200; i++ {
		parts := make([]string, 0, 16)
		for j := 0; j < 16; j++ {
			parts = append(parts, fmt.Sprintf("%v", float32(rng.NormFloat64())))
		}
		checkAgainstReference(t, "["+strings.Join(parts, ",")+"]")
		checkAgainstReference(t, "["+strings.Join(parts, ",")+",oops]")
	}
}

// TestParseVectorFloat32Semantics pins the float32 conversion behaviour that
// the engine depends on: rounding to float32 and rejection of overflow.
func TestParseVectorFloat32Semantics(t *testing.T) {
	// 3.4028235e38 is the largest finite float32; one ULP above overflows.
	got, err := ParseVector("[3.4028235e38]")
	if err != nil {
		t.Fatalf("largest float32 rejected: %v", err)
	}
	if math.Float32bits(got[0]) != math.Float32bits(float32(3.4028235e38)) {
		t.Fatalf("largest float32 mismatch: %v", got[0])
	}
	if _, err := ParseVector("[3.4028236e38]"); err == nil {
		t.Fatal("float32 overflow must be rejected")
	} else if err.Name != "invalid_vector" {
		t.Fatalf("unexpected error name %q", err.Name)
	}
	// NaN and Inf literals are rejected by the format gate.
	for _, s := range []string{"[nan]", "[inf]", "[Infinity]"} {
		if _, err := ParseVector(s); err == nil {
			t.Fatalf("%s must be rejected", s)
		}
	}
}

// TestEightDigitsBlock checks the eight-digit block conversion against the
// obvious loop, and the digit test against a brute force check.
func TestEightDigitsBlock(t *testing.T) {
	rng := rand.New(rand.NewSource(23))
	for i := 0; i < 20000; i++ {
		n := rng.Intn(100000000)
		s := fmt.Sprintf("%08d", n)
		v := le64(s, 0)
		if !eightDigits(v) {
			t.Fatalf("%q is not eight digits", s)
		}
		if got := eightDigitsValue(v); got != uint64(n) {
			t.Fatalf("%q: got %d want %d", s, got, n)
		}
	}
	// Blocks that are not eight digits must be rejected.
	for _, s := range []string{"1234567a", "1234567 ", "1234567+", "1234567.",
		"-1234567", "1234567.5", "\x80\x80\x80\x80\x80\x80\x80\x80",
		"\xff\xff\xff\xff\xff\xff\xff\xff", "        ", "\n\n\n\n\n\n\n\n"} {
		if eightDigits(le64(s, 0)) {
			t.Fatalf("%q must not be eight digits", s)
		}
	}
	// The block must agree with the byte-wise reading.
	for i := 0; i < 1000; i++ {
		var b [8]byte
		for j := range b {
			b[j] = byte('0' + rng.Intn(10))
		}
		v := le64(string(b[:]), 0)
		if !eightDigits(v) {
			t.Fatalf("%q is not eight digits", b)
		}
		var want uint64
		for j := range b {
			want = want*10 + uint64(b[j]-'0')
		}
		if got := eightDigitsValue(v); got != want {
			t.Fatalf("%q: got %d want %d", b, got, want)
		}
	}
}

// TestParseFloatFastMatchesStrconv checks the fast decimal scanner against
// strconv on every shape it is meant to accept: whenever it returns a value,
// that value has to be the one strconv produces, bit for bit.
func TestParseFloatFastMatchesStrconv(t *testing.T) {
	rng := rand.New(rand.NewSource(17))
	check := func(s string) {
		t.Helper()
		got, ok := parseFloatFast(s)
		if !ok {
			return
		}
		want, err := strconv.ParseFloat(s, 64)
		if err != nil {
			t.Fatalf("%q: fast path accepted, strconv failed: %v", s, err)
		}
		if math.Float64bits(got) != math.Float64bits(want) {
			t.Fatalf("%q: got %v (%x) want %v (%x)", s, got, math.Float64bits(got), want, math.Float64bits(want))
		}
	}
	// Random decimals with a wide range of digit counts and exponents.
	for i := 0; i < 40000; i++ {
		n := rng.Intn(1000000)
		digits := fmt.Sprintf("%d", n)
		switch rng.Intn(4) {
		case 0:
			digits += fmt.Sprintf("%06d", rng.Intn(1000000))
		case 1:
			digits += fmt.Sprintf("%d", rng.Intn(100))
		case 2:
			digits += fmt.Sprintf("%09d", rng.Intn(1000000000))
		}
		if rng.Intn(2) == 0 {
			digits = "0" + digits
		}
		if rng.Intn(2) == 0 {
			digits = digits[:1] + "." + digits[1:]
		}
		if rng.Intn(2) == 0 {
			digits = "-" + digits
		}
		if rng.Intn(3) == 0 {
			digits += fmt.Sprintf("e%+d", rng.Intn(46)-23)
		}
		check(digits)
		check(strings.TrimPrefix(digits, "-"))
	}
	for _, s := range []string{"0", "-0", "+0", "0.0", "1", "1.", ".5", "-.5",
		"0.5", "00.50", "1e0", "1E0", "1e+5", "1e-5", "1e22", "1e-22", "1e23",
		"1e-23", "1e308", "1e-308", "1e-323", "1e-324", "1e-400",
		"1.7976931348623157e308", "900719925474099", "9007199254740991",
		"9007199254740992", "9007199254740993", "12345678901234567",
		"123456789012345678901234567890", "0.00000000000000000001"} {
		check(s)
	}
}

// TestTrimSpaceMatchesStdlib checks the inlined fast path against
// strings.TrimSpace. The vector parser trims every element, so a fast path
// that is not exactly equivalent would change which literals are accepted.
func TestTrimSpaceMatchesStdlib(t *testing.T) {
	cases := []string{
		"", " ", "  ", "\t", "\n", "\r", "\v", "\f", "a", " a", "a ", " a ",
		"\u00a0", "\u00a0a", "a\u00a0", "a\u00a0 ", " \u00a0 ",
		"\u3000", "a\u3000", "\u3000a", "a\u2003b", "1", "1.5", "-1e5",
		"0.12345678901234567", " 0.12345678901234567 ",
	}
	for _, s := range cases {
		if got, want := trimSpace(s), strings.TrimSpace(s); got != want {
			t.Fatalf("trimSpace(%q) = %q, want %q", s, got, want)
		}
	}
	rng := rand.New(rand.NewSource(4242))
	alphabet := []rune{' ', '\t', '\n', '\r', '\v', '\f', 'a', '1', '.', 'e',
		'-', '\u00a0', '\u2003', '\u3000', '\u2028'}
	for i := 0; i < 100000; i++ {
		n := rng.Intn(8)
		b := make([]rune, n)
		for j := range b {
			b[j] = alphabet[rng.Intn(len(alphabet))]
		}
		s := string(b)
		if got, want := trimSpace(s), strings.TrimSpace(s); got != want {
			t.Fatalf("trimSpace(%q) = %q, want %q", s, got, want)
		}
	}
}

// BenchmarkParseVector measures literal parsing alone.
func BenchmarkParseVector(b *testing.B) {
	rng := rand.New(rand.NewSource(3))
	lit := make([]string, 128)
	for i := range lit {
		lit[i] = fmt.Sprintf("%v", float32(rng.NormFloat64()))
	}
	s := "[" + strings.Join(lit, ",") + "]"
	b.SetBytes(int64(len(s)))
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		v, err := ParseVector(s)
		if err != nil || len(v) != 128 {
			b.Fatal(err)
		}
	}
}

// BenchmarkParseVectorWorkload measures the literal shape the benchmark
// client sends: a float32 widened to float64 and printed with the shortest
// decimal that round trips, which is up to 17 significant digits.
func BenchmarkParseVectorWorkload(b *testing.B) {
	rng := rand.New(rand.NewSource(3))
	lit := make([]string, 128)
	for i := range lit {
		lit[i] = strconv.FormatFloat(float64(float32(rng.NormFloat64())), 'f', -1, 64)
	}
	s := "[" + strings.Join(lit, ",") + "]"
	b.SetBytes(int64(len(s)))
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		v, err := ParseVector(s)
		if err != nil || len(v) != 128 {
			b.Fatal(err)
		}
	}
}
