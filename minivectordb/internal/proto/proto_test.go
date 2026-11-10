package proto

import (
	"bytes"
	"encoding/json"
	"reflect"
	"testing"

	"minivectordb/internal/engine"
	"minivectordb/internal/types"
)

// decode checks that a response is exactly one strict JSON line.
func decode(t *testing.T, b []byte) map[string]any {
	t.Helper()
	if len(b) == 0 || b[len(b)-1] != '\n' {
		t.Fatalf("response must end with newline: %q", b)
	}
	for _, c := range b[:len(b)-1] {
		if c == '\n' {
			t.Fatalf("response spans several lines: %q", b)
		}
	}
	var m map[string]any
	d := json.NewDecoder(bytes.NewReader(b))
	d.UseNumber()
	if err := d.Decode(&m); err != nil {
		t.Fatalf("invalid JSON %q: %v", b, err)
	}
	return m
}

func TestReady(t *testing.T) {
	if Ready != "{\"type\":\"ready\"}\n" {
		t.Fatalf("ready line %q", Ready)
	}
}

func TestRequestIDEchoedVerbatim(t *testing.T) {
	for _, id := range []string{`7`, `"7"`, `12345678901234567890`} {
		req, err := ParseRequest([]byte(`{"id":` + id + `,"op":"execute","sql":"SELECT 1;","search_mode":"ann"}` + "\n"))
		if err != nil {
			t.Fatal(err)
		}
		if *req.SQL != "SELECT 1;" || req.SearchMode != "ann" || req.Op != "execute" {
			t.Fatalf("bad decode %+v", req)
		}
		var e Encoder
		got := string(e.OK(req.ID))
		if want := `{"id":` + id + `,"ok":true}` + "\n"; got != want {
			t.Fatalf("got %q want %q", got, want)
		}
	}
	if _, err := ParseRequest([]byte(`{"op":"shutdown"}`)); err == nil {
		t.Fatal("missing id accepted")
	}
}

func TestResultShapes(t *testing.T) {
	var e Encoder
	id := json.RawMessage(`3`)
	q := &engine.Result{IsQuery: true, Columns: []string{"id", "s", "v", "d"}, Rows: [][]types.Value{
		{types.Int(9223372036854775807), types.Str("a\"\n你"), types.Vec([]float32{0.1, 3e38}), types.Float(5)},
	}}
	m := decode(t, e.Result(id, q))
	if m["ok"] != true || m["id"] != json.Number("3") {
		t.Fatalf("head %v", m)
	}
	row := m["rows"].([]any)[0].([]any)
	if row[0] != json.Number("9223372036854775807") || row[1] != "a\"\n你" {
		t.Fatalf("row %v", row)
	}
	if !reflect.DeepEqual(row[2], []any{json.Number("0.1"), json.Number("3e+38")}) {
		t.Fatalf("vector %v", row[2])
	}
	if row[3] != json.Number("5.0") {
		t.Fatalf("distance %v", row[3])
	}

	empty := decode(t, e.Result(id, &engine.Result{IsQuery: true, Columns: []string{"id"}}))
	if rows, ok := empty["rows"].([]any); !ok || len(rows) != 0 {
		t.Fatalf("empty rows %v", empty)
	}

	w := decode(t, e.Result(id, &engine.Result{Affected: 2, HasAffected: true}))
	if w["affected_rows"] != json.Number("2") || w["ok"] != true {
		t.Fatalf("write %v", w)
	}

	f := decode(t, e.SQLError(id, types.ErrDupKey(1)))
	errObj := f["error"].(map[string]any)
	if f["ok"] != false || errObj["code"] != "duplicate_key" || errObj["message"] == "" {
		t.Fatalf("error %v", f)
	}
	bad := decode(t, e.Error(id, "", "x\xff"))
	if bad["error"].(map[string]any)["code"] != "internal_error" {
		t.Fatalf("empty code %v", bad)
	}
}
