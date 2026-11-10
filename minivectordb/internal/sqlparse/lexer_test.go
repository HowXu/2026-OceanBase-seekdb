package sqlparse

import "testing"

// TestScanString covers the no-copy fast path and the escaping slow path.
func TestScanString(t *testing.T) {
	cases := []struct{ in, want string }{
		{`'abc'`, "abc"},
		{`''`, ""},
		{`'[1, 2.5, -3e2]'`, "[1, 2.5, -3e2]"},
		{`'你好'`, "你好"},
		{`'it''s'`, "it's"},
		{`'a\nb'`, "a\nb"},
		{`'a\\b'`, `a\b`},
		{`'x\'y'`, "x'y"},
		{`"say ""hi"""`, `say "hi"`},
		{`"a'b"`, "a'b"},
	}
	for _, c := range cases {
		got, end, err := scanString(c.in+" rest", 0)
		if err != nil || got != c.want || end != len(c.in) {
			t.Errorf("%s: got %q end %d err %v, want %q end %d", c.in, got, end, err, c.want, len(c.in))
		}
	}
	for _, bad := range []string{`'abc`, `'it''s`, `'a\'`} {
		if _, _, err := scanString(bad, 0); err == nil {
			t.Errorf("%s: expected unterminated error", bad)
		}
	}
}
