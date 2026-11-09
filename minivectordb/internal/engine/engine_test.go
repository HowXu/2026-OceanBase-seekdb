package engine

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"minivectordb/internal/storage"
	"minivectordb/internal/types"
)

func open(t *testing.T, dir string) *DB {
	t.Helper()
	db, err := Open(dir, Options{Sync: storage.SyncNone})
	if err != nil {
		t.Fatal(err)
	}
	return db
}

func mustExec(t *testing.T, db *DB, sql string) *Result {
	t.Helper()
	r, err := db.Exec(sql)
	if err != nil {
		t.Fatalf("%s: %v", sql, err)
	}
	return r
}

func errName(err error) string {
	if se, ok := err.(*types.SQLError); ok {
		return se.Name
	}
	return fmt.Sprint(err)
}

// snapshot renders all rows of docs in pk order.
func snapshot(t *testing.T, db *DB) string {
	t.Helper()
	if db.tables["docs"] == nil {
		return "<none>"
	}
	r := mustExec(t, db, "SELECT * FROM docs ORDER BY id")
	b, _ := json.Marshal(r.Rows)
	return string(b)
}

func TestStatementAtomicityAndTxn(t *testing.T) {
	db := open(t, t.TempDir())
	mustExec(t, db, "CREATE TABLE docs (id BIGINT PRIMARY KEY, c VARCHAR(4), e VECTOR(2))")
	mustExec(t, db, "INSERT INTO docs VALUES (1,'a','[0,0]'),(2,'b','[1,0]')")
	cases := []struct{ sql, err string }{
		{"INSERT INTO docs VALUES (3,'c','[0,0]'),(1,'x','[0,0]')", "duplicate_key"},
		{"INSERT INTO docs VALUES (3,'c','[0,0]'),(4,'x','[0]')", "dimension_mismatch"},
		{"INSERT INTO docs VALUES (3,'c','[0,0]'),(4,'xxxxx','[0,0]')", "data_too_long"},
		{"INSERT INTO docs VALUES (3,'c','[0,0]'),(3,'d','[0,0]')", "duplicate_key"},
		{"INSERT INTO docs VALUES (3,'c','[NaN,0]')", "invalid_vector"},
		{"INSERT INTO docs VALUES (3,'c','[1e39,0]')", "invalid_vector"},
		{"INSERT INTO docs VALUES (3,'c','[inf,0]')", "invalid_vector"},
		{"UPDATE docs SET e = '[1,2,3]' WHERE id >= 1", "dimension_mismatch"},
		{"UPDATE docs SET id = 5 WHERE id = 1", "unsupported_sql"},
		{"SELECT * FROM docs WHERE nope = 1", "column_not_found"},
		{"SELECT COUNT(*) FROM docs", "unsupported_sql"},
		{"SELECT * FROM docs WHERE id = 1 OR id = 2", "unsupported_sql"},
	}
	before := snapshot(t, db)
	for _, c := range cases {
		_, err := db.Exec(c.sql)
		if got := errName(err); got != c.err {
			t.Errorf("%s: got %s, want %s", c.sql, got, c.err)
		}
	}
	if s := snapshot(t, db); s != before {
		t.Fatalf("failed statements changed data: %s", s)
	}
	mustExec(t, db, "BEGIN")
	mustExec(t, db, "UPDATE docs SET c = 'z' WHERE id = 1")
	mustExec(t, db, "DELETE FROM docs WHERE id = 2")
	mustExec(t, db, "INSERT INTO docs VALUES (2,'n','[5,5]')")
	if _, err := db.Exec("INSERT INTO docs VALUES (7,'q','[0,0]'),(2,'q','[0,0]')"); errName(err) != "duplicate_key" {
		t.Fatal(err)
	}
	r := mustExec(t, db, "SELECT id, L2_DISTANCE(e, '[5,5]') AS d FROM docs ORDER BY d LIMIT 1")
	if r.Rows[0][0].I != 2 || r.Rows[0][1].F != 0 {
		t.Fatalf("txn did not see own write: %+v", r.Rows)
	}
	mustExec(t, db, "ROLLBACK")
	if s := snapshot(t, db); s != before {
		t.Fatalf("rollback: %s want %s", s, before)
	}
}

// TestCrashAtEveryOffset writes a history of committed and uncommitted work,
// then reopens copies of the log cut at every byte. The recovered state must
// equal the state after some committed prefix, and must contain everything
// committed before the cut point.
func TestCrashAtEveryOffset(t *testing.T) {
	dir := t.TempDir()
	db := open(t, dir)
	states := []string{"<none>"}
	ends := []int64{0}
	step := func(sqls ...string) {
		for _, s := range sqls {
			mustExec(t, db, s)
		}
		db.log.Flush()
		states = append(states, snapshot(t, db))
		ends = append(ends, db.log.Size())
	}
	step("CREATE TABLE docs (id BIGINT PRIMARY KEY, c VARCHAR(8), e VECTOR(2))")
	step("INSERT INTO docs VALUES (1,'a','[0,0]'),(2,'b','[1,1]'),(3,'c','[2,2]')")
	step("CREATE VECTOR INDEX vi ON docs(e)")
	step("BEGIN", "UPDATE docs SET c='x' WHERE id <= 2", "DELETE FROM docs WHERE id = 3", "COMMIT")
	step("BEGIN", "INSERT INTO docs VALUES (9,'r','[9,9]')", "ROLLBACK")
	step("UPDATE docs SET e='[7,7]' WHERE id = 1")
	// Uncommitted tail: written to the log but never committed.
	for _, s := range []string{"BEGIN", "INSERT INTO docs VALUES (4,'d','[4,4]')", "DELETE FROM docs WHERE id = 1"} {
		mustExec(t, db, s)
	}
	db.log.Flush()
	full, err := os.ReadFile(filepath.Join(dir, logName))
	if err != nil {
		t.Fatal(err)
	}
	for cut := 0; cut <= len(full); cut++ {
		d := t.TempDir()
		if err := os.WriteFile(filepath.Join(d, logName), full[:cut], 0o644); err != nil {
			t.Fatal(err)
		}
		for round := 0; round < 2; round++ { // reopening twice checks idempotent recovery
			rdb := open(t, d)
			got := snapshot(t, rdb)
			want := ""
			for i := range ends {
				if ends[i] <= int64(cut) {
					want = states[i]
				}
			}
			if got != want {
				t.Fatalf("cut %d round %d: got %s want %s", cut, round, got, want)
			}
			if rdb.tables["docs"] != nil {
				r := mustExec(t, rdb, "SELECT id FROM docs ORDER BY L2_DISTANCE(e,'[0,0]') LIMIT 10")
				if len(r.Rows) != len(mustExec(t, rdb, "SELECT id FROM docs").Rows) {
					t.Fatalf("cut %d: index and table disagree", cut)
				}
			}
			rdb.Close()
		}
	}
}

// TestCompaction forces a log rewrite and checks that data, the vector index
// and later writes survive it, including a reopen after more changes.
func TestCompaction(t *testing.T) {
	old := compactMinGarbage
	compactMinGarbage = 1
	defer func() { compactMinGarbage = old }()
	dir := t.TempDir()
	db := open(t, dir)
	mustExec(t, db, "CREATE TABLE docs (id BIGINT PRIMARY KEY, c VARCHAR(8), e VECTOR(2))")
	mustExec(t, db, "CREATE TABLE other (id BIGINT PRIMARY KEY)")
	mustExec(t, db, "INSERT INTO other VALUES (7)")
	mustExec(t, db, "CREATE VECTOR INDEX vi ON docs(e)")
	for i := 0; i < 50; i++ {
		mustExec(t, db, fmt.Sprintf("INSERT INTO docs VALUES (%d,'v','[%d,0]')", i, i))
	}
	for i := 0; i < 50; i += 2 {
		mustExec(t, db, fmt.Sprintf("UPDATE docs SET c='u%d' WHERE id = %d", i, i))
	}
	mustExec(t, db, "DELETE FROM docs WHERE id >= 40")
	want := snapshot(t, db)
	size := db.log.Size()
	db.Close()

	db = open(t, dir)
	if db.log.Size() >= size {
		t.Fatalf("log not compacted: %d >= %d", db.log.Size(), size)
	}
	if got := snapshot(t, db); got != want {
		t.Fatalf("after compaction: %s", got)
	}
	if r := mustExec(t, db, "SELECT id FROM other"); len(r.Rows) != 1 {
		t.Fatal("other table lost")
	}
	r := mustExec(t, db, "SELECT id FROM docs ORDER BY L2_DISTANCE(e, '[39,0]') LIMIT 1")
	if r.Rows[0][0].I != 39 {
		t.Fatalf("index after compaction: %v", r.Rows)
	}
	mustExec(t, db, "INSERT INTO docs VALUES (100,'n','[100,0]')")
	want = snapshot(t, db)
	db.Close()
	db = open(t, dir)
	if got := snapshot(t, db); got != want {
		t.Fatalf("reopen after compaction: %s", got)
	}
	if db.byID[db.tables["docs"].ID].Index == nil || !db.tables["docs"].Index.Explicit {
		t.Fatal("explicit index lost in compaction")
	}
	db.Close()
}
