package sqlparse

import (
	"math/rand"
	"strings"
	"testing"
)

// scanStringReference is the byte-at-a-time implementation that scanString
// replaced. It stays in the tree as the behavioural oracle: the vectorised
// scan has to return the same text, the same end offset and the same error
// for every input.
func scanStringReference(src string, i int) (string, int, error) {
	q := src[i]
	for j := i + 1; j < len(src); j++ {
		c := src[j]
		if c == '\\' {
			break
		}
		if c == q {
			if j+1 < len(src) && src[j+1] == q {
				break
			}
			return src[i+1 : j], j + 1, nil
		}
	}
	var sb strings.Builder
	j := i + 1
	n := len(src)
	for {
		if j >= n {
			return "", 0, syntaxErr("unterminated string literal")
		}
		c := src[j]
		if c == q {
			if j+1 < n && src[j+1] == q {
				sb.WriteByte(q)
				j += 2
				continue
			}
			return sb.String(), j + 1, nil
		}
		if c == '\\' && j+1 < n {
			e := src[j+1]
			switch e {
			case 'n':
				sb.WriteByte('\n')
			case 't':
				sb.WriteByte('\t')
			case 'r':
				sb.WriteByte('\r')
			case 'b':
				sb.WriteByte('\b')
			case '0':
				sb.WriteByte(0)
			case 'Z':
				sb.WriteByte(26)
			case '%', '_':
				sb.WriteByte('\\')
				sb.WriteByte(e)
			default:
				sb.WriteByte(e)
			}
			j += 2
			continue
		}
		sb.WriteByte(c)
		j++
	}
}

// TestScanStringMatchesReference checks the vectorised scan against the
// byte-at-a-time implementation it replaced, over random bodies that mix
// quotes, escapes and plain text.
func TestScanStringMatchesReference(t *testing.T) {
	rng := rand.New(rand.NewSource(24680))
	alphabet := []byte("ab'\"\\ ntrZ0%_[],.0123")
	pick := func(n int) []byte {
		b := make([]byte, n)
		for j := range b {
			b[j] = alphabet[rng.Intn(len(alphabet))]
		}
		return b
	}
	for i := 0; i < 200000; i++ {
		q := byte('\'')
		if rng.Intn(2) == 0 {
			q = '"'
		}
		src := string(q) + string(pick(rng.Intn(24))) + string(q) + string(pick(rng.Intn(8)))
		got, gotEnd, gotErr := scanString(src, 0)
		want, wantEnd, wantErr := scanStringReference(src, 0)
		if (gotErr == nil) != (wantErr == nil) {
			t.Fatalf("%q: error mismatch: got %v want %v", src, gotErr, wantErr)
		}
		if gotErr != nil {
			ge, gok := gotErr.(*Error)
			we, wok := wantErr.(*Error)
			if gok != wok {
				t.Fatalf("%q: error type mismatch: got %T want %T", src, gotErr, wantErr)
			}
			if gok && ge.Msg != we.Msg {
				t.Fatalf("%q: error text mismatch: got %q want %q", src, ge.Msg, we.Msg)
			}
			continue
		}
		if got != want || gotEnd != wantEnd {
			t.Fatalf("%q: got %q end %d, want %q end %d", src, got, gotEnd, want, wantEnd)
		}
	}
}

// TestScanStringLongLiteral measures the shape the insert path sends: a long
// vector literal with no escapes.
func TestScanStringLongLiteral(t *testing.T) {
	var sb strings.Builder
	sb.WriteByte('\'')
	for i := 0; i < 128; i++ {
		if i > 0 {
			sb.WriteByte(',')
		}
		sb.WriteString("0.12345678901234567")
	}
	sb.WriteByte('\'')
	src := sb.String()
	got, end, err := scanString(src, 0)
	if err != nil {
		t.Fatalf("scanString failed: %v", err)
	}
	if end != len(src) {
		t.Fatalf("end offset %d, want %d", end, len(src))
	}
	want, _, _ := scanStringReference(src, 0)
	if got != want {
		t.Fatal("long literal mismatch")
	}
}
