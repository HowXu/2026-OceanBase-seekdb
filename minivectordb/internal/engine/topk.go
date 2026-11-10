package engine

import (
	"container/heap"
	"math"
	"runtime"
	"sort"
	"sync"
)

const ivfMinRows = 200000

// parallelMinSlots is the smallest index for which the exact scan is split
// across goroutines; below it the coordination costs more than it saves.
// A variable so tests can force either path.
var parallelMinSlots = 4096

type hit struct {
	d   float64
	pk  int64
	off int64
}

// worse reports whether a ranks after b (distance, then primary key).
func worse(a, b hit) bool {
	if a.d != b.d {
		return a.d > b.d
	}
	return a.pk > b.pk
}

type hitHeap []hit // max-heap: the worst hit on top

func (h hitHeap) Len() int           { return len(h) }
func (h hitHeap) Less(i, j int) bool { return worse(h[i], h[j]) }
func (h hitHeap) Swap(i, j int)      { h[i], h[j] = h[j], h[i] }
func (h *hitHeap) Push(x any)        { *h = append(*h, x.(hit)) }
func (h *hitHeap) Pop() any {
	o := *h
	x := o[len(o)-1]
	*h = o[:len(o)-1]
	return x
}

type topK struct {
	k    int
	h    hitHeap
	pred func([]byte) bool
	db   *DB
}

// bound is the distance a candidate must not exceed to possibly enter.
func (t *topK) bound() float64 {
	if len(t.h) < t.k {
		return math.Inf(1)
	}
	return t.h[0].d
}

// offer adds a candidate. Filtering must happen before heap admission: a row
// that does not satisfy WHERE must not consume one of the TopK slots or make
// a valid filtered row look too far away to enter.
func (t *topK) offer(d float64, pk, off int64) {
	if t.pred != nil && !t.pred(t.db.rowBytes(off)) {
		return
	}
	t.admit(d, pk, off)
}

// admit adds a candidate that is already known to satisfy WHERE.
func (t *topK) admit(d float64, pk, off int64) {
	c := hit{d, pk, off}
	full := len(t.h) >= t.k
	if full && !worse(t.h[0], c) {
		return
	}
	if full {
		t.h[0] = c
		heap.Fix(&t.h, 0)
	} else {
		heap.Push(&t.h, c)
	}
}

func (t *topK) result() []int64 {
	hs := []hit(t.h)
	sort.Slice(hs, func(i, j int) bool { return worse(hs[j], hs[i]) })
	out := make([]int64, len(hs))
	for i, h := range hs {
		out[i] = h.off
	}
	return out
}

// vectorTopK answers ORDER BY L2_DISTANCE(col, q) LIMIT k from the vector
// index. It returns false when no index can be used, in which case the
// caller falls back to a generic sorted scan with identical semantics.
func (db *DB) vectorTopK(t *Table, q []float32, pred func([]byte) bool, k int, approx bool) ([]int64, bool) {
	if t.Index == nil {
		// Without CREATE VECTOR INDEX, keep an implicit in-memory copy so
		// repeated TopK queries do not re-read rows from the log.
		t.Index = newVecIndex("")
		t.Index.rebuild(db, t)
	}
	x := t.Index
	if x.Disabled {
		return nil, false
	}
	ann := approx || db.opts.SearchMode == "ann"
	// IVF is only worth its recall risk on large tables. Measured locally on
	// uniform random 128-dim data, nprobe=nlist/4 gave Recall@10 of 0.61 at
	// 10k rows while saving only ~30% query time, so below ivfMinRows the
	// "ann" mode answers with the exact scan.
	ann = ann && x.live >= ivfMinRows
	if ann && x.Explicit && x.live >= 2*x.ivfBuilt {
		x.trainIVF() // first use, or the table has doubled since training
	}
	var ov map[int64]int64
	if db.txn != nil {
		ov = db.txn.overlay[t.ID]
	}
	visit := func(tk *topK, s int32) {
		pk := x.pks[s]
		if pk == tombstone {
			return
		}
		if ov != nil {
			if _, shadowed := ov[pk]; shadowed {
				return
			}
		}
		// Evaluate WHERE before the distance: a filtered-out row costs one
		// row lookup instead of a full distance computation.
		off := x.off[s]
		if pred != nil && !pred(db.rowBytes(off)) {
			return
		}
		bd := tk.bound()
		d := l2sqBound(q, x.vec(s), bd)
		if d > bd {
			return
		}
		tk.admit(d, pk, off)
	}
	// ANN only applies to an explicit, trained index. Filtered queries and
	// queries inside a transaction with pending changes stay exact.
	useIVF := ann && x.nlist > 0 && pred == nil &&
		len(ov) == 0 && k <= x.live/16
	tk := &topK{k: k, pred: pred, db: db, h: make(hitHeap, 0, k+1)}
	if useIVF {
		for _, c := range x.probeOrder(q)[:x.nprobe()] {
			for _, s := range x.lists[c] {
				visit(tk, s)
			}
		}
	} else if workers := scanWorkers(len(x.pks)); workers > 1 {
		// Row bytes may be read concurrently only from the kernel mapping,
		// so push buffered log bytes out first; View on flushed data is
		// then read-only.
		if err := db.log.Flush(); err != nil {
			panic(err)
		}
		db.log.Prepare()
		parts := make([]*topK, workers)
		var wg sync.WaitGroup
		n := len(x.pks)
		for w := 0; w < workers; w++ {
			lo, hi := n*w/workers, n*(w+1)/workers
			p := &topK{k: k, pred: pred, db: db, h: make(hitHeap, 0, k+1)}
			parts[w] = p
			wg.Add(1)
			go func() {
				defer wg.Done()
				for s := lo; s < hi; s++ {
					visit(p, int32(s))
				}
			}()
		}
		wg.Wait()
		// Merging the per-part heaps with the same (distance, pk) order
		// yields exactly the serial TopK.
		for _, p := range parts {
			for _, h := range p.h {
				tk.admit(h.d, h.pk, h.off)
			}
		}
	} else {
		for s := range x.pks {
			visit(tk, int32(s))
		}
	}
	for pk, off := range ov {
		if off != deleted {
			tk.offer(l2sqBytes(q, db.rowBytes(off)[t.vecOff:]), pk, off)
		}
	}
	return tk.result(), true
}

func scanWorkers(slots int) int {
	if slots < parallelMinSlots {
		return 1
	}
	w := runtime.GOMAXPROCS(0)
	if w > 2 {
		w = 2
	}
	return w
}
