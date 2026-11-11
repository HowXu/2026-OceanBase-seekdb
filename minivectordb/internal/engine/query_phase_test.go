package engine

import (
	"fmt"
	"math/rand"
	"os"
	"strings"
	"testing"
	"time"

	sp "minivectordb/internal/sqlparse"
	"minivectordb/internal/storage"
)

// TestPhaseBreakdown times the stages of the benchmark query workload in
// process: SQL parse (incl. vector literal), full Exec, and the share of
// Exec spent in the vector scan. Diagnostic only.
func TestPhaseBreakdown(t *testing.T) {
	if os.Getenv("MVDB_PHASES") == "" {
		t.Skip("set MVDB_PHASES=1")
	}
	dir := t.TempDir()
	db, err := Open(dir, Options{Sync: storage.SyncNone})
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	rng := rand.New(rand.NewSource(7))
	const rows, dim, cats = 10000, 128, 10
	vec := func() string {
		var b strings.Builder
		b.WriteByte('[')
		for i := 0; i < dim; i++ {
			if i > 0 {
				b.WriteByte(',')
			}
			fmt.Fprintf(&b, "%v", float32(rng.Float64()))
		}
		b.WriteByte(']')
		return b.String()
	}
	mustExec(t, db, fmt.Sprintf("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(16), embedding VECTOR(%d))", dim))
	for s := 0; s < rows; s += 100 {
		var b strings.Builder
		b.WriteString("INSERT INTO docs VALUES ")
		for i := s; i < s+100; i++ {
			if i > s {
				b.WriteByte(',')
			}
			fmt.Fprintf(&b, "(%d,'c%d','%s')", i+1, rng.Intn(cats), vec())
		}
		mustExec(t, db, b.String())
	}
	var qs []string
	for j := 0; j < 1000; j++ {
		where := ""
		if j%2 == 1 {
			where = fmt.Sprintf(" WHERE category = 'c%d'", rng.Intn(cats))
		}
		qs = append(qs, fmt.Sprintf("SELECT id, L2_DISTANCE(embedding, '%s') AS distance FROM docs%s ORDER BY distance LIMIT 10", vec(), where))
	}
	mustExec(t, db, qs[0])

	t0 := time.Now()
	for _, q := range qs {
		if _, err := sp.Parse(q); err != nil {
			t.Fatal(err)
		}
	}
	parse := time.Since(t0)

	t0 = time.Now()
	for _, q := range qs {
		mustExec(t, db, q)
	}
	exec := time.Since(t0)

	t0 = time.Now()
	for j, q := range qs {
		if j%2 == 0 {
			mustExec(t, db, q)
		}
	}
	unf := time.Since(t0)
	t0 = time.Now()
	for j, q := range qs {
		if j%2 == 1 {
			mustExec(t, db, q)
		}
	}
	fil := time.Since(t0)
	fmt.Printf("PHASES queries=1000 parse_ms=%.1f exec_ms=%.1f unfiltered500_ms=%.1f filtered500_ms=%.1f\n",
		ms(parse), ms(exec), ms(unf), ms(fil))
}

func ms(d time.Duration) float64 { return float64(d.Microseconds()) / 1000 }
