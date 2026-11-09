package engine

import (
	"container/heap"
	"math"
	"sort"
	"strings"

	sp "minivectordb/internal/sqlparse"
	"minivectordb/internal/types"
)

type outCol struct {
	name string
	c    *compiled
}

type sortKey struct {
	c    *compiled
	desc bool
}

func (db *DB) execSelect(s *sp.Select) (*Result, error) {
	var t *Table
	if s.Table != "" {
		var err *types.SQLError
		if t, err = db.table(s.Table); err != nil {
			return nil, err
		}
	}
	sc := &scope{t: t, alias: s.Alias}
	var outs []outCol
	itemOut := make([]int, len(s.Items))
	for idx, it := range s.Items {
		itemOut[idx] = len(outs)
		if it.Star {
			if t == nil {
				return nil, types.ErrSyntax("SELECT * requires a FROM clause")
			}
			for _, c := range t.Cols {
				cc, _ := sc.compile(&sp.ColRef{Name: c.Name})
				outs = append(outs, outCol{c.Name, cc})
			}
			continue
		}
		c, err := sc.compile(it.Expr)
		if err != nil {
			return nil, err
		}
		name := it.Alias
		if name == "" {
			if cr, ok := it.Expr.(*sp.ColRef); ok && c.col >= 0 {
				name = cr.Name
			} else {
				name = it.Text
			}
		}
		outs = append(outs, outCol{name, c})
	}
	keys, err := db.orderKeys(s, sc, outs, itemOut)
	if err != nil {
		return nil, err
	}
	res := &Result{IsQuery: true}
	for _, o := range outs {
		res.Columns = append(res.Columns, o.name)
		res.Kinds = append(res.Kinds, o.c.kind)
	}
	if t == nil {
		if s.Where != nil {
			p, err := sc.pred(s.Where)
			if err != nil {
				return nil, err
			}
			if !p(nil) {
				return res, nil
			}
		}
		if s.Limit == 0 || s.Offset > 0 {
			return res, nil
		}
		row := make([]types.Value, len(outs))
		for i, o := range outs {
			row[i] = o.c.fn(nil)
		}
		res.Rows = [][]types.Value{row}
		return res, nil
	}
	var pred func([]byte) bool
	if s.Where != nil {
		p, perr := sc.pred(s.Where)
		if perr != nil {
			return nil, perr
		}
		pred = p
	}
	limit := s.Limit
	want := int64(math.MaxInt64)
	if limit >= 0 {
		if limit > math.MaxInt64-s.Offset {
			want = math.MaxInt64
		} else {
			want = limit + s.Offset
		}
	}
	if want == 0 {
		return res, nil
	}
	var offs []int64
	ok := false
	if len(keys) == 1 && keys[0].c.l2q != nil && !keys[0].desc && want <= 1<<20 {
		offs, ok = db.vectorTopK(t, keys[0].c.l2q, pred, int(want), s.Approx)
	}
	if !ok {
		offs = db.scanSorted(t, pred, keys, want)
	}
	if int64(len(offs)) <= s.Offset {
		return res, nil
	}
	offs = offs[s.Offset:]
	res.Rows = make([][]types.Value, len(offs))
	for i, off := range offs {
		b := db.rowBytes(off)
		row := make([]types.Value, len(outs))
		for j, o := range outs {
			row[j] = o.c.fn(b)
		}
		res.Rows[i] = row
	}
	return res, nil
}

// orderKeys resolves ORDER BY items: select aliases first, then ordinals,
// then arbitrary expressions over the table.
func (db *DB) orderKeys(s *sp.Select, sc *scope, outs []outCol, itemOut []int) ([]sortKey, error) {
	var keys []sortKey
	for _, oi := range s.Order {
		var c *compiled
		switch e := oi.Expr.(type) {
		case *sp.Ordinal:
			if e.N < 1 || int(e.N) > len(outs) {
				return nil, types.ErrAmbiguousOrder(itoa(e.N))
			}
			c = outs[e.N-1].c
		case *sp.ColRef:
			if e.Table == "" {
				for i, it := range s.Items {
					if it.Alias != "" && strings.EqualFold(it.Alias, e.Name) {
						c = outs[itemOut[i]].c
						break
					}
				}
			}
		}
		if c == nil {
			var err *types.SQLError
			if c, err = sc.compile(oi.Expr); err != nil {
				if cr, ok := oi.Expr.(*sp.ColRef); ok && err.Code == 1054 {
					return nil, types.ErrAmbiguousOrder(cr.Name)
				}
				return nil, err
			}
		}
		if c.kind == types.KVec {
			return nil, types.ErrTypeMismatch("Cannot ORDER BY a vector")
		}
		keys = append(keys, sortKey{c, oi.Desc})
	}
	return keys, nil
}

func itoa(n int64) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	var b [20]byte
	i := len(b)
	for n > 0 {
		i--
		b[i] = byte('0' + n%10)
		n /= 10
	}
	if neg {
		i--
		b[i] = '-'
	}
	return string(b[i:])
}

// cand is a row candidate carrying its evaluated sort keys.
type cand struct {
	pk   int64
	off  int64
	keys []types.Value
}

// less orders by the sort keys, then by primary key ascending.
func lessCand(keys []sortKey) func(a, b *cand) bool {
	return func(a, b *cand) bool {
		for i, k := range keys {
			c := compareValues(a.keys[i], b.keys[i])
			if c != 0 {
				if k.desc {
					return c > 0
				}
				return c < 0
			}
		}
		return a.pk < b.pk
	}
}

type candHeap struct {
	items []*cand
	less  func(a, b *cand) bool
}

func (h *candHeap) Len() int           { return len(h.items) }
func (h *candHeap) Less(i, j int) bool { return h.less(h.items[j], h.items[i]) } // max-heap
func (h *candHeap) Swap(i, j int)      { h.items[i], h.items[j] = h.items[j], h.items[i] }
func (h *candHeap) Push(x any)         { h.items = append(h.items, x.(*cand)) }
func (h *candHeap) Pop() any {
	n := len(h.items)
	x := h.items[n-1]
	h.items = h.items[:n-1]
	return x
}

// scanSorted filters all visible rows and returns the first want offsets in
// sort order. Without ORDER BY rows are returned in primary key order.
func (db *DB) scanSorted(t *Table, pred func([]byte) bool, keys []sortKey, want int64) []int64 {
	less := lessCand(keys)
	h := &candHeap{less: less}
	bounded := want < int64(db.visibleCount(t))
	db.forEach(t, func(pk, off int64) bool {
		b := db.rowBytes(off)
		if pred != nil && !pred(b) {
			return true
		}
		c := &cand{pk: pk, off: off, keys: make([]types.Value, len(keys))}
		for i, k := range keys {
			c.keys[i] = k.c.fn(b)
		}
		if !bounded {
			h.items = append(h.items, c)
			return true
		}
		if int64(h.Len()) < want {
			heap.Push(h, c)
		} else if less(c, h.items[0]) {
			h.items[0] = c
			heap.Fix(h, 0)
		}
		return true
	})
	items := h.items
	sort.Slice(items, func(i, j int) bool { return less(items[i], items[j]) })
	if int64(len(items)) > want {
		items = items[:want]
	}
	out := make([]int64, len(items))
	for i, c := range items {
		out[i] = c.off
	}
	return out
}
