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

// TestWritePhases splits the 100 x 100-row INSERT workload into SQL parse
// (including vector literals) and full Exec, without fsync. Diagnostic only;
// enabled with MVDB_PHASES=1.
func TestWritePhases(t *testing.T) {
	if os.Getenv("MVDB_PHASES") == "" {
		t.Skip("set MVDB_PHASES=1")
	}
	rng := rand.New(rand.NewSource(7))
	var batches []string
	for s := 0; s < 10000; s += 100 {
		var b strings.Builder
		b.WriteString("INSERT INTO docs VALUES ")
		for i := s; i < s+100; i++ {
			if i > s {
				b.WriteByte(',')
			}
			fmt.Fprintf(&b, "(%d,'c%d','[", i+1, rng.Intn(10))
			for j := 0; j < 128; j++ {
				if j > 0 {
					b.WriteByte(',')
				}
				fmt.Fprintf(&b, "%v", float64(float32(rng.Float64())))
			}
			b.WriteString("]')")
		}
		batches = append(batches, b.String())
	}
	t0 := time.Now()
	for _, s := range batches {
		if _, err := sp.Parse(s); err != nil {
			t.Fatal(err)
		}
	}
	parse := time.Since(t0)
	db, err := Open(t.TempDir(), Options{Sync: storage.SyncNone})
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	mustExec(t, db, "CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(16), embedding VECTOR(128))")
	t0 = time.Now()
	for _, s := range batches {
		mustExec(t, db, s)
	}
	exec := time.Since(t0)
	fmt.Printf("WRITEPHASES batches=100 parse_ms=%.1f exec_ms=%.1f sql_bytes=%d\n",
		float64(parse.Microseconds())/1000, float64(exec.Microseconds())/1000, len(strings.Join(batches, "")))
}
