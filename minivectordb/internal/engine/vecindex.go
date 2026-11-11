package engine

import (
	"math"
	"math/rand"
	"sort"

	"minivectordb/internal/types"
)

// VecIndex keeps a contiguous in-memory copy of the committed vectors of one
// table. Exact search scans it linearly; ANN search uses an IVF partition
// built over the same slots. It is derived state: recovery rebuilds it from
// the log, so it is never persisted.
type VecIndex struct {
	Name     string
	Explicit bool // created by CREATE VECTOR INDEX
	// Disabled is set when the vectors do not fit in the memory budget; the
	// index then holds nothing and queries scan the log instead.
	Disabled bool
	dim      int
	pks      []int64
	vecs     []float32 // len(pks)*dim
	off      []int64   // row offset of each slot
	pos      map[int64]int32
	free     []int32
	live     int

	// IVF state. list[c] holds slot ids; slotList[s] is the list of slot s.
	cents    []float32
	nlist    int
	lists    [][]int32
	slotList []int32
	ivfBuilt int // live count when the IVF was trained

	// gen changes whenever a slot changes; derived sets compare against it.
	gen uint64
	eq  map[int]*eqSet
}

const tombstone int64 = math.MinInt64

func newVecIndex(name string) *VecIndex {
	return &VecIndex{Name: name, pos: map[int64]int32{}}
}

func (x *VecIndex) rebuild(db *DB, t *Table) {
	x.gen++
	x.dim = t.Dim
	x.Disabled = int64(len(t.rows))*int64(t.Dim)*4 > db.opts.IndexMemBytes
	if x.Disabled {
		x.pks, x.vecs, x.off, x.free, x.pos, x.live = nil, nil, nil, nil, map[int64]int32{}, 0
		x.resetIVF()
		return
	}
	x.pks, x.vecs, x.off, x.free = nil, nil, nil, nil
	x.pos = make(map[int64]int32, len(t.rows))
	x.live = 0
	x.resetIVF()
	x.pks = make([]int64, 0, len(t.rows))
	x.vecs = make([]float32, 0, len(t.rows)*t.Dim)
	x.off = make([]int64, 0, len(t.rows))
	for pk, off := range t.rows {
		x.upsert(db, t, pk, off)
	}
}

func (x *VecIndex) resetIVF() {
	x.cents, x.lists, x.slotList, x.nlist, x.ivfBuilt = nil, nil, nil, 0, 0
}

func (x *VecIndex) upsert(db *DB, t *Table, pk, off int64) {
	if x.Disabled {
		return
	}
	x.gen++
	if !x.has(pk) && int64(x.live+1)*int64(x.dim)*4 > db.opts.IndexMemBytes {
		x.Disabled = true
		x.pks, x.vecs, x.off, x.free, x.pos, x.live = nil, nil, nil, nil, map[int64]int32{}, 0
		x.resetIVF()
		return
	}
	b := db.rowBytes(off)
	s, ok := x.pos[pk]
	if !ok {
		if n := len(x.free); n > 0 {
			s = x.free[n-1]
			x.free = x.free[:n-1]
		} else {
			s = int32(len(x.pks))
			x.pks = append(x.pks, 0)
			x.off = append(x.off, 0)
			x.vecs = append(x.vecs, make([]float32, x.dim)...)
			if x.slotList != nil {
				x.slotList = append(x.slotList, -1)
			}
		}
		x.pos[pk] = s
		x.live++
	} else if x.slotList != nil {
		x.unlist(s)
	}
	x.pks[s] = pk
	x.off[s] = off
	v := x.vecs[int(s)*x.dim : int(s+1)*x.dim]
	types.DecodeVecInto(v, b[t.vecOff:])
	if x.slotList != nil {
		c := x.nearestCentroid(v)
		x.lists[c] = append(x.lists[c], s)
		x.slotList[s] = int32(c)
	}
}

func (x *VecIndex) has(pk int64) bool { _, ok := x.pos[pk]; return ok }

func (x *VecIndex) remove(pk int64) {
	s, ok := x.pos[pk]
	if !ok {
		return
	}
	x.gen++
	delete(x.pos, pk)
	if x.slotList != nil {
		x.unlist(s)
	}
	x.pks[s] = tombstone
	x.free = append(x.free, s)
	x.live--
}

func (x *VecIndex) unlist(s int32) {
	c := x.slotList[s]
	if c < 0 {
		return
	}
	l := x.lists[c]
	for i, v := range l {
		if v == s {
			l[i] = l[len(l)-1]
			x.lists[c] = l[:len(l)-1]
			break
		}
	}
	x.slotList[s] = -1
}

// All distance kernels share one summation order, so a TopK ordering key and
// the L2_DISTANCE value printed for the same row are bit-identical:
// differences are taken in float64; element i of each 16-element block goes
// to accumulator i%8; elements after the last full block go to accumulator 0
// in order; the result is ((s0+s1)+(s2+s3))+((s4+s5)+(s6+s7)).

// widen converts a query vector to float64 once per query.
func widen(q []float32) []float64 {
	w := make([]float64, len(q))
	for i, f := range q {
		w[i] = float64(f)
	}
	return w
}

// l2sq is the squared Euclidean distance in float64.
func l2sq(a, b []float32) float64 { return l2sqBound(widen(a), b, math.Inf(1)) }

func (x *VecIndex) vec(s int32) []float32 { return x.vecs[int(s)*x.dim : int(s+1)*x.dim] }

// l2sqBound computes the squared distance between the widened query a and
// the stored vector b, but stops after a 16-element block once the partial
// sum exceeds bound, returning a value > bound. Every term is non-negative,
// so the partial sum only grows and the early exit never drops a true
// candidate. When the full sum is computed it equals l2sq exactly.
func l2sqBound(a []float64, b []float32, bound float64) float64 {
	b = b[:len(a)]
	if useSIMD {
		// The vector kernel has no early exit; it is faster than the
		// scalar loop even when the scalar loop stops early.
		return l2sqSIMD(a, b)
	}
	var s0, s1, s2, s3, s4, s5, s6, s7 float64
	i := 0
	for ; i+16 <= len(a); i += 16 {
		x := (*[16]float64)(a[i:])
		y := (*[16]float32)(b[i:])
		d0 := x[0] - float64(y[0])
		d1 := x[1] - float64(y[1])
		d2 := x[2] - float64(y[2])
		d3 := x[3] - float64(y[3])
		d4 := x[4] - float64(y[4])
		d5 := x[5] - float64(y[5])
		d6 := x[6] - float64(y[6])
		d7 := x[7] - float64(y[7])
		s0 += d0 * d0
		s1 += d1 * d1
		s2 += d2 * d2
		s3 += d3 * d3
		s4 += d4 * d4
		s5 += d5 * d5
		s6 += d6 * d6
		s7 += d7 * d7
		d0 = x[8] - float64(y[8])
		d1 = x[9] - float64(y[9])
		d2 = x[10] - float64(y[10])
		d3 = x[11] - float64(y[11])
		d4 = x[12] - float64(y[12])
		d5 = x[13] - float64(y[13])
		d6 = x[14] - float64(y[14])
		d7 = x[15] - float64(y[15])
		s0 += d0 * d0
		s1 += d1 * d1
		s2 += d2 * d2
		s3 += d3 * d3
		s4 += d4 * d4
		s5 += d5 * d5
		s6 += d6 * d6
		s7 += d7 * d7
		if p := ((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7)); p > bound {
			return p
		}
	}
	for ; i < len(a); i++ {
		d := a[i] - float64(b[i])
		s0 += d * d
	}
	return ((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7))
}

func (x *VecIndex) nearestCentroid(v []float32) int {
	best, bd := 0, math.Inf(1)
	for c := 0; c < x.nlist; c++ {
		if d := l2sq(v, x.cents[c*x.dim:(c+1)*x.dim]); d < bd {
			best, bd = c, d
		}
	}
	return best
}

// trainIVF runs a few k-means iterations on a sample of live vectors.
func (x *VecIndex) trainIVF() {
	x.resetIVF()
	if x.live < 2048 {
		return
	}
	nlist := int(math.Sqrt(float64(x.live)))
	if nlist > 4096 {
		nlist = 4096
	}
	slots := make([]int32, 0, x.live)
	for s, pk := range x.pks {
		if pk != tombstone {
			slots = append(slots, int32(s))
		}
	}
	rng := rand.New(rand.NewSource(42))
	sample := slots
	if len(sample) > nlist*64 {
		sample = make([]int32, nlist*64)
		for i, j := range rng.Perm(len(slots))[:len(sample)] {
			sample[i] = slots[j]
		}
	}
	d := x.dim
	cents := make([]float32, nlist*d)
	for c, j := range rng.Perm(len(sample))[:nlist] {
		copy(cents[c*d:], x.vec(sample[j]))
	}
	x.cents, x.nlist = cents, nlist
	assign := make([]int, len(sample))
	sums := make([]float64, nlist*d)
	counts := make([]int, nlist)
	for iter := 0; iter < 8; iter++ {
		for i, s := range sample {
			assign[i] = x.nearestCentroid(x.vec(s))
		}
		for i := range sums {
			sums[i] = 0
		}
		for i := range counts {
			counts[i] = 0
		}
		for i, s := range sample {
			c := assign[i]
			counts[c]++
			for k, f := range x.vec(s) {
				sums[c*d+k] += float64(f)
			}
		}
		for c := 0; c < nlist; c++ {
			if counts[c] == 0 {
				copy(cents[c*d:], x.vec(sample[rng.Intn(len(sample))]))
				continue
			}
			for k := 0; k < d; k++ {
				cents[c*d+k] = float32(sums[c*d+k] / float64(counts[c]))
			}
		}
	}
	x.lists = make([][]int32, nlist)
	x.slotList = make([]int32, len(x.pks))
	for i := range x.slotList {
		x.slotList[i] = -1
	}
	for _, s := range slots {
		c := x.nearestCentroid(x.vec(s))
		x.lists[c] = append(x.lists[c], s)
		x.slotList[s] = int32(c)
	}
	x.ivfBuilt = x.live
}

// nprobe is deliberately generous: recall must stay >= 0.95 even on
// unclustered data, where IVF partitions are least effective.
func (x *VecIndex) nprobe() int {
	n := x.nlist / 2
	if n < 16 {
		n = 16
	}
	if n > x.nlist {
		n = x.nlist
	}
	return n
}

// probeOrder returns list ids sorted by centroid distance to q.
func (x *VecIndex) probeOrder(q []float32) []int {
	type cd struct {
		c int
		d float64
	}
	ds := make([]cd, x.nlist)
	for c := 0; c < x.nlist; c++ {
		ds[c] = cd{c, l2sq(q, x.cents[c*x.dim:(c+1)*x.dim])}
	}
	sort.Slice(ds, func(i, j int) bool { return ds[i].d < ds[j].d })
	out := make([]int, x.nlist)
	for i, e := range ds {
		out[i] = e.c
	}
	return out
}
