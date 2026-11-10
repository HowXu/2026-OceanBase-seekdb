package proto

import (
	"bytes"
	"encoding/json"
	"errors"
)

// Request is one JSONL request from the judge, e.g.
// {"id":1,"op":"execute","sql":"SELECT 1;","search_mode":"exact"}.
type Request struct {
	// ID is echoed verbatim so its JSON type (number, string) is preserved.
	ID         json.RawMessage `json:"id"`
	Op         string          `json:"op"`
	SQL        *string         `json:"sql"`
	SearchMode string          `json:"search_mode"`
}

// ParseRequest decodes one request line. On error the returned request still
// carries the id when it could be recovered, so the reply can echo it.
func ParseRequest(line []byte) (*Request, error) {
	line = bytes.TrimSpace(line)
	var r Request
	if err := json.Unmarshal(line, &r); err != nil {
		// Recover the id from an otherwise malformed object, if possible.
		var probe struct {
			ID json.RawMessage `json:"id"`
		}
		if json.Unmarshal(line, &probe) == nil {
			r.ID = probe.ID
		}
		return &r, err
	}
	if len(r.ID) == 0 || bytes.Equal(r.ID, []byte("null")) {
		return &r, errors.New("missing request id")
	}
	return &r, nil
}
