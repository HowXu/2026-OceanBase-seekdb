package engine

import (
	"fmt"
	"math"
	"math/rand"
	"sort"
	"strings"
	"testing"
)

// TestFilteredTopKMatchesReference checks vector TopK with and without WHERE
// against an independent brute-force reference: float64 squared distance over
// the float32 vectors, ties by primary key, filter applied first. Rows change
// through UPDATE/DELETE and an open transaction so the overlay path is
// covered too. It runs once with the serial scan and once with the scan split
// across goroutines.
func TestFilteredTopKMatchesReference(t *testing.T) {
	for _, min := range []int{1 << 30, 1} {
		parallelMinSlots = min
		testFilteredTopK(t)
	}
	parallelMinSlots = 4096
}

func testFilteredTopK(t *testing.T) {
	db := open(t, t.TempDir())
	rng := rand.New(rand.NewSource(11))
	const dim, n = 8, 600
	cats := []string{"a", "b", "ab", "你好", "z"}
	type row struct {
		id  int64
		cat string
		v   []float32
	}
	rows := map[int64]row{}
	vecLit := func(v []float32) string {
		p := make([]string, len(v))
		for i, f := range v {
			p[i] = fmt.Sprint(f)
		}
		return "[" + strings.Join(p, ",") + "]"
	}
	randVec := func() []float32 {
		v := make([]float32, dim)
		for i := range v {
			// Coarse values create many exact distance ties.
			v[i] = float32(rng.Intn(5))
		}
		return v
	}
	mustExec(t, db, fmt.Sprintf("CREATE TABLE docs (id BIGINT PRIMARY KEY, cat VARCHAR(8), e VECTOR(%d))", dim))
	for i := int64(1); i <= n; i++ {
		r := row{i, cats[rng.Intn(len(cats))], randVec()}
		rows[i] = r
		mustExec(t, db, fmt.Sprintf("INSERT INTO docs VALUES (%d,'%s','%s')", i, r.cat, vecLit(r.v)))
	}
	for i := 0; i < 80; i++ {
		id := int64(rng.Intn(n) + 1)
		if _, ok := rows[id]; !ok {
			continue
		}
		if i%4 == 0 {
			mustExec(t, db, fmt.Sprintf("DELETE FROM docs WHERE id = %d", id))
			delete(rows, id)
		} else {
			r := row{id, cats[rng.Intn(len(cats))], randVec()}
			rows[id] = r
			mustExec(t, db, fmt.Sprintf("UPDATE docs SET cat = '%s', e = '%s' WHERE id = %d", r.cat, vecLit(r.v), id))
		}
	}
	ops := []string{"=", "<>", "<", ">=", "=", "const=", "and"}
	check := func(tag string) {
		for qn := 0; qn < 60; qn++ {
			q := randVec()
			k := 1 + rng.Intn(25)
			where, keep := "", func(row) bool { return true }
			if qn%3 != 0 {
				op, c := ops[rng.Intn(len(ops))], cats[rng.Intn(len(cats))]
				if qn%11 == 0 {
					c = "none" // matches no row
				}
				switch op {
				case "const=":
					where = fmt.Sprintf(" WHERE '%s' = cat", c)
					keep = func(r row) bool { return r.cat == c }
				case "and":
					lim := int64(rng.Intn(n))
					where = fmt.Sprintf(" WHERE id > %d AND cat = '%s'", lim, c)
					keep = func(r row) bool { return r.id > lim && r.cat == c }
				default:
					where = fmt.Sprintf(" WHERE cat %s '%s'", op, c)
					keep = func(r row) bool {
						cmp := strings.Compare(r.cat, c)
						return map[string]bool{"=": cmp == 0, "<>": cmp != 0, "<": cmp < 0, ">=": cmp >= 0}[op]
					}
				}
			}
			type cand struct {
				d  float64
				id int64
			}
			var ref []cand
			for id, r := range rows {
				if !keep(r) {
					continue
				}
				var s float64
				for i := range q {
					d := float64(q[i]) - float64(r.v[i])
					s += d * d
				}
				ref = append(ref, cand{s, id})
			}
			sort.Slice(ref, func(i, j int) bool {
				if ref[i].d != ref[j].d {
					return ref[i].d < ref[j].d
				}
				return ref[i].id < ref[j].id
			})
			if len(ref) > k {
				ref = ref[:k]
			}
			sql := fmt.Sprintf("SELECT id, L2_DISTANCE(e, '%s') AS d FROM docs%s ORDER BY d LIMIT %d", vecLit(q), where, k)
			res := mustExec(t, db, sql)
			if len(res.Rows) != len(ref) {
				t.Fatalf("%s: %s: got %d rows, want %d", tag, sql, len(res.Rows), len(ref))
			}
			for i, c := range ref {
				got := res.Rows[i]
				if got[0].I != c.id || math.Abs(got[1].F-math.Sqrt(c.d)) > 1e-9 {
					t.Fatalf("%s: %s: rank %d got (%d, %v) want (%d, %v)", tag, sql, i, got[0].I, got[1].F, c.id, math.Sqrt(c.d))
				}
			}
		}
	}
	check("committed")

	mustExec(t, db, "BEGIN")
	for i := 0; i < 40; i++ {
		id := int64(rng.Intn(n+50) + 1)
		r := row{id, cats[rng.Intn(len(cats))], randVec()}
		if _, ok := rows[id]; ok {
			if i%3 == 0 {
				mustExec(t, db, fmt.Sprintf("DELETE FROM docs WHERE id = %d", id))
				delete(rows, id)
				continue
			}
			mustExec(t, db, fmt.Sprintf("UPDATE docs SET cat = '%s', e = '%s' WHERE id = %d", r.cat, vecLit(r.v), id))
		} else {
			mustExec(t, db, fmt.Sprintf("INSERT INTO docs VALUES (%d,'%s','%s')", id, r.cat, vecLit(r.v)))
		}
		rows[id] = r
	}
	check("in transaction")
	mustExec(t, db, "ROLLBACK")
}
