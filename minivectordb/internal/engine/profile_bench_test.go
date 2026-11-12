package engine

import (
	"fmt"
	"math/rand"
	"os"
	"runtime/pprof"
	"strings"
	"testing"

	sp "minivectordb/internal/sqlparse"
	"minivectordb/internal/storage"
)

// buildWriteWorkload makes the same 100 x 100-row INSERT workload the lab and
// the phase test use, so a profile of the write path is representative.
func buildWriteWorkload() []string {
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
	return batches
}

// BenchmarkWriteProfile profiles only the Exec loop, so the workload
// construction does not pollute the profile.
func BenchmarkWriteProfile(b *testing.B) {
	batches := buildWriteWorkload()
	f, err := os.Create("/tmp/write.prof")
	if err != nil {
		b.Fatal(err)
	}
	defer f.Close()
	db, err := Open(b.TempDir(), Options{Sync: storage.SyncNone})
	if err != nil {
		b.Fatal(err)
	}
	defer db.Close()
	mustExecB(b, db, "CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(16), embedding VECTOR(128))")
	b.ResetTimer()
	if err := pprof.StartCPUProfile(f); err != nil {
		b.Fatal(err)
	}
	for it := 0; it < b.N; it++ {
		for _, s := range batches {
			if _, err := db.Exec(s); err != nil {
				b.Fatal(err)
			}
		}
	}
	pprof.StopCPUProfile()
	b.StopTimer()
}

// BenchmarkParseProfile profiles only the SQL parse loop.
func BenchmarkParseProfile(b *testing.B) {
	batches := buildWriteWorkload()
	f, err := os.Create("/tmp/parse.prof")
	if err != nil {
		b.Fatal(err)
	}
	defer f.Close()
	b.ResetTimer()
	if err := pprof.StartCPUProfile(f); err != nil {
		b.Fatal(err)
	}
	for it := 0; it < b.N; it++ {
		for _, s := range batches {
			if _, err := sp.Parse(s); err != nil {
				b.Fatal(err)
			}
		}
	}
	pprof.StopCPUProfile()
	b.StopTimer()
}

// BenchmarkQueryProfile profiles only the query loop against a loaded table.
func BenchmarkQueryProfile(b *testing.B) {
	batches := buildWriteWorkload()
	db, err := Open(b.TempDir(), Options{Sync: storage.SyncNone})
	if err != nil {
		b.Fatal(err)
	}
	defer db.Close()
	mustExecB(b, db, "CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(16), embedding VECTOR(128))")
	for _, s := range batches {
		if _, err := db.Exec(s); err != nil {
			b.Fatal(err)
		}
	}
	q := make([]float32, 128)
	rng := rand.New(rand.NewSource(9))
	for i := range q {
		q[i] = rng.Float32()
	}
	f, err := os.Create("/tmp/query.prof")
	if err != nil {
		b.Fatal(err)
	}
	defer f.Close()
	b.ResetTimer()
	if err := pprof.StartCPUProfile(f); err != nil {
		b.Fatal(err)
	}
	for it := 0; it < b.N; it++ {
		for i := 0; i < 500; i++ {
			if _, err := db.Exec("SELECT id, L2_DISTANCE(embedding, '" + vectorText(q) + "') AS distance FROM docs ORDER BY distance LIMIT 10"); err != nil {
				b.Fatal(err)
			}
		}
	}
	pprof.StopCPUProfile()
	b.StopTimer()
}

func vectorText(q []float32) string {
	var b strings.Builder
	b.WriteByte('[')
	for i, v := range q {
		if i > 0 {
			b.WriteByte(',')
		}
		fmt.Fprintf(&b, "%v", float64(v))
	}
	b.WriteByte(']')
	return b.String()
}

func mustExecB(b *testing.B, db *DB, sql string) {
	b.Helper()
	if _, err := db.Exec(sql); err != nil {
		b.Fatal(err)
	}
}
