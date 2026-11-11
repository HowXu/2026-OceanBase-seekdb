package proto

import (
	"bytes"
	"encoding/json"
	"math/rand"
	"testing"
)

// slowParse is the reference: the encoding/json path of ParseRequest.
func slowParse(line []byte) (*Request, error) {
	line = bytes.TrimSpace(line)
	var r Request
	if err := json.Unmarshal(line, &r); err != nil {
		return nil, err
	}
	if len(r.ID) == 0 || bytes.Equal(r.ID, []byte("null")) {
		return nil, errMissingID
	}
	return &r, nil
}

func same(a, b *Request) bool {
	if !bytes.Equal(a.ID, b.ID) || a.Op != b.Op || a.SearchMode != b.SearchMode {
		return false
	}
	if (a.SQL == nil) != (b.SQL == nil) {
		return false
	}
	return a.SQL == nil || *a.SQL == *b.SQL
}

// TestFastParseMatchesEncodingJSON checks that whenever the fast path accepts
// a line, encoding/json accepts it with the same result.
func TestFastParseMatchesEncodingJSON(t *testing.T) {
	cases := []string{
		`{"id":1,"op":"execute","sql":"SELECT 1;","search_mode":"exact"}`,
		` { "id" : "abc" , "op" : "shutdown" } `,
		`{"op":"execute","id":-0,"sql":"INSERT INTO t VALUES (1,'a;b','[1,2]')"}`,
		`{"id":12345678901234567890,"op":"execute","sql":"你好"}`,
		`{"id":1,"op":"execute","sql":"a\"b"}`,
		`{"id":1,"op":"execute","sql":"a\nb"}`,
		`{"id":01,"op":"x"}`,
		`{"id":1.5,"op":"x"}`,
		`{"id":null,"op":"x"}`,
		`{"id":1,"id":2,"op":"x"}`,
		`{"id":1,"op":"x","extra":true}`,
		`{"id":1,"op":"x",}`,
		`{"id":1,"op":"x"} trailing`,
		`{}`,
		`{"op":"x"}`,
		"{\"id\":1,\"op\":\"\xff\"}",
		`{"id":1,"op":"execute","sql":"x","search_mode":"ann"}`,
	}
	rng := rand.New(rand.NewSource(1))
	alphabet := []byte(`{}":,\ abc01-9.enul` + "\t\xff")
	for i := 0; i < 20000; i++ {
		b := []byte(cases[rng.Intn(len(cases))])
		for m := rng.Intn(3); m > 0; m-- {
			p := rng.Intn(len(b) + 1)
			switch rng.Intn(3) {
			case 0:
				b = append(b[:p], append([]byte{alphabet[rng.Intn(len(alphabet))]}, b[p:]...)...)
			case 1:
				if p < len(b) {
					b = append(b[:p], b[p+1:]...)
				}
			default:
				if p < len(b) {
					b[p] = alphabet[rng.Intn(len(alphabet))]
				}
			}
		}
		cases = append(cases, string(b))
	}
	accepted := 0
	for _, c := range cases {
		fr, ok := parseFast(bytes.TrimSpace([]byte(c)))
		if !ok {
			continue
		}
		accepted++
		sr, err := slowParse([]byte(c))
		if err != nil {
			t.Fatalf("%q: fast accepted, encoding/json rejected: %v", c, err)
		}
		if !same(fr, sr) {
			t.Fatalf("%q: fast %+v != slow %+v", c, fr, sr)
		}
	}
	if accepted < 100 {
		t.Fatalf("fast path accepted only %d cases", accepted)
	}
}
