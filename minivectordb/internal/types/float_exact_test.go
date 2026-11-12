package types

import (
	"math"
	"math/rand"
	"strconv"
	"strings"
	"testing"
)

// checkFastAgainstStrconv enforces the invariant of the fast path: whenever it
// accepts a literal it returns exactly the bits strconv returns.
//
// strconv rounds to nearest with ties to even, so a literal that is converted
// exactly has to produce the same float64. Any difference is a rounding bug
// and would move the distances that the query planner ranks by.
func checkFastAgainstStrconv(t *testing.T, s string) bool {
	t.Helper()
	fast, ok := parseFloatFast(s)
	if !ok {
		return false
	}
	want, err := strconv.ParseFloat(s, 64)
	if err != nil {
		t.Fatalf("parseFloatFast(%q) accepted a literal strconv rejects: %v", s, err)
	}
	if math.Float64bits(fast) != math.Float64bits(want) {
		t.Fatalf("parseFloatFast(%q) = %v (%016x), strconv = %v (%016x)",
			s, fast, math.Float64bits(fast), want, math.Float64bits(want))
	}
	return true
}

// randomLiteral builds a decimal literal in one of the shapes a client can
// send: an optional sign, an optional integer part, an optional fraction and
// an optional exponent.
func randomLiteral(r *rand.Rand) string {
	var sb strings.Builder
	switch r.Intn(6) {
	case 0:
		sb.WriteByte('-')
	case 1:
		sb.WriteByte('+')
	}
	intDigits := r.Intn(20)
	fracDigits := r.Intn(21)
	for i := 0; i < intDigits; i++ {
		sb.WriteByte(byte('0' + r.Intn(10)))
	}
	if r.Intn(8) != 0 {
		sb.WriteByte('.')
		for i := 0; i < fracDigits; i++ {
			sb.WriteByte(byte('0' + r.Intn(10)))
		}
	}
	if r.Intn(3) == 0 {
		sb.WriteByte('e')
		if r.Intn(2) == 0 {
			sb.WriteByte('-')
		}
		sb.WriteString(strconv.Itoa(r.Intn(40)))
	}
	return sb.String()
}

// TestParseFloatFastMatchesStrconvRandom sweeps random literals of every
// shape and compares the fast path against strconv bit for bit.
func TestParseFloatFastMatchesStrconvRandom(t *testing.T) {
	r := rand.New(rand.NewSource(20261111))
	accepted := 0
	for i := 0; i < 3000000; i++ {
		if checkFastAgainstStrconv(t, randomLiteral(r)) {
			accepted++
		}
	}
	if accepted == 0 {
		t.Fatal("the fast path never accepted a literal")
	}
}

// TestParseFloatFastMatchesStrconvDigits walks every digit count and every
// fraction length, which is where the rounding boundaries sit.
func TestParseFloatFastMatchesStrconvDigits(t *testing.T) {
	for intDigits := 0; intDigits <= 19; intDigits++ {
		for fracDigits := 0; fracDigits <= 19; fracDigits++ {
			for _, lead := range []string{"1", "9", "3", "7"} {
				var sb strings.Builder
				sb.WriteString(lead)
				for i := 1; i < intDigits; i++ {
					sb.WriteByte(byte('0' + (i*7)%10))
				}
				if fracDigits > 0 {
					sb.WriteByte('.')
					for i := 0; i < fracDigits; i++ {
						sb.WriteByte(byte('0' + (i*3+1)%10))
					}
				}
				checkFastAgainstStrconv(t, sb.String())
			}
		}
	}
}

// TestParseFloatFastMatchesStrconvExponents covers the exponent range around
// the boundaries of the exact conversion.
func TestParseFloatFastMatchesStrconvExponents(t *testing.T) {
	bases := []string{"1", "1.5", "9.87654321", "12345678901234567", "0.000000000000000001"}
	for _, b := range bases {
		for exp := -320; exp <= 320; exp++ {
			s := b + "e" + strconv.Itoa(exp)
			checkFastAgainstStrconv(t, s)
		}
	}
}

// TestParseFloatFastWorkloadLiterals uses the shape the benchmark client
// sends: a float32 widened to float64 and printed with the shortest decimal
// that round trips, which is up to 17 significant digits.
func TestParseFloatFastWorkloadLiterals(t *testing.T) {
	r := rand.New(rand.NewSource(987654321))
	for i := 0; i < 200000; i++ {
		f := float32(r.Float64())
		v := float64(f)
		checkFastAgainstStrconv(t, strconv.FormatFloat(v, 'f', -1, 64))
		checkFastAgainstStrconv(t, strconv.FormatFloat(v, 'g', -1, 64))
		checkFastAgainstStrconv(t, strconv.FormatFloat(v, 'e', -1, 64))
	}
}

// TestParseFloatFastLongExponent pins the exponent handling. An exponent with
// more than four digits used to be truncated, so "1e00005" was read as 1.
func TestParseFloatFastLongExponent(t *testing.T) {
	cases := map[string]float64{
		"1e00005":   100000,
		"1e5":       100000,
		"1.5e00003": 1500,
	}
	for s, want := range cases {
		got, ok := parseFloatFast(s)
		if !ok {
			t.Fatalf("parseFloatFast(%q) refused a literal it can convert", s)
		}
		if got != want {
			t.Fatalf("parseFloatFast(%q) = %v, want %v", s, got, want)
		}
	}
	// An exponent with more than nine digits is refused, and ParseVector
	// then falls back to strconv, which still gets it right.
	v, err := ParseVector("[1e0000000000000000000005]")
	if err != nil {
		t.Fatalf("ParseVector failed: %v", err)
	}
	if float64(v[0]) != 1e5 {
		t.Fatalf("ParseVector([1e0000000000000000000005]) = %v, want 1e5", v[0])
	}
}

// TestDecimalExactRoundingBoundary pins the rounding decisions on literals
// that sit exactly on a tie, where the round-to-nearest-even rule matters.
func TestDecimalExactRoundingBoundary(t *testing.T) {
	cases := []string{
		"0.5", "1.5", "2.5", "3.5",
		"9007199254740993", "9007199254740995",
		"1.0000000000000002", "1.0000000000000004",
		"2.2250738585072011e-308", "2.2250738585072014e-308",
		"4.9406564584124654e-324",
		"1.7976931348623157e308",
	}
	for _, s := range cases {
		checkFastAgainstStrconv(t, s)
	}
}

// TestDecimalExactCarryToFiftyThreeBits covers the one branch the rest of the
// suite never reaches: the rounding that carries a significand up to 2^53.
//
// The literal has to sit on or just above the midpoint between the two
// float64 values around 2^53, so that the significand comes out as all ones
// and the carry moves it into the next exponent. The random literals never
// land there, so without this test a regression in that branch would move a
// value by one ulp and silently change the distances that the planner ranks
// by.
func TestDecimalExactCarryToFiftyThreeBits(t *testing.T) {
	cases := map[string]float64{
		// Exactly on the midpoint below 2^53: ties go to even, so the
		// significand is all ones and has to be carried.
		"9007199254740991.5":   9007199254740992,
		"90071992547409915e-1": 9007199254740992,
		// Just above the midpoint: the same carry without a tie.
		"9007199254740991.6": 9007199254740992,
		// Just below the midpoint: no carry, the significand stays.
		"9007199254740991.4": 9007199254740991,
	}
	for s, want := range cases {
		got, ok := parseFloatFast(s)
		if !ok {
			t.Fatalf("parseFloatFast(%q) refused a literal it can convert", s)
		}
		if got != want {
			t.Fatalf("parseFloatFast(%q) = %v, want %v", s, got, want)
		}
		checkFastAgainstStrconv(t, s)
	}
}
