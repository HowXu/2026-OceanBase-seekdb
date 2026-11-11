package engine

import (
	sp "minivectordb/internal/sqlparse"
	"minivectordb/internal/types"
)

// eqSet groups the index slots of committed rows by the value of one VARCHAR
// column. It is derived state, built on first use and dropped whenever the
// index changes (gen), so it needs no maintenance on the write path.
type eqSet struct {
	gen uint64
	m   map[string][]int32
}

// eqSlots returns the slots whose column col equals v. The second result is
// false when no candidate set can be used.
func (x *VecIndex) eqSlots(db *DB, t *Table, col int, v string) ([]int32, bool) {
	if x.Disabled || col < 0 {
		return nil, false
	}
	if x.eq == nil {
		x.eq = map[int]*eqSet{}
	}
	e := x.eq[col]
	if e == nil || e.gen != x.gen {
		e = &eqSet{gen: x.gen, m: map[string][]int32{}}
		for s, pk := range x.pks {
			if pk == tombstone {
				continue
			}
			k := string(t.varBytes(db.rowBytes(x.off[s]), col))
			e.m[k] = append(e.m[k], int32(s))
		}
		x.eq[col] = e
	}
	return e.m[v], true
}

// eqCovers reports whether the candidate set alone decides WHERE: the whole
// condition is the single equality on the set's column.
func eqCovers(e sp.Expr) bool {
	x, ok := e.(*sp.Cmp)
	return ok && x.Op == "="
}

// eqFilter finds a conjunct "varchar_column = 'constant'" in WHERE. The full
// WHERE is still evaluated on every candidate, so this only narrows the scan.
func eqFilter(sc *scope, e sp.Expr) (col int, val string, ok bool) {
	switch x := e.(type) {
	case *sp.And:
		if col, val, ok = eqFilter(sc, x.L); ok {
			return
		}
		return eqFilter(sc, x.R)
	case *sp.Cmp:
		if x.Op != "=" {
			return -1, "", false
		}
		c, l := x.L.(*sp.ColRef)
		s, r := x.R.(*sp.StrLit)
		if !l || !r {
			c, l = x.R.(*sp.ColRef)
			s, r = x.L.(*sp.StrLit)
		}
		if !l || !r || sc.t == nil {
			return -1, "", false
		}
		i, err := sc.resolve(c)
		if err != nil || sc.t.Cols[i].Type != types.TVarchar {
			return -1, "", false
		}
		return i, s.V, true
	}
	return -1, "", false
}
