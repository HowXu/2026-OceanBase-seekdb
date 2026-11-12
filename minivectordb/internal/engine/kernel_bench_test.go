package engine

import (
	"encoding/binary"
	"math"
	"math/rand"
	"testing"
)

func kernelData() (q []float32, vs []float32, n, dim int) {
	rng := rand.New(rand.NewSource(1))
	n, dim = 10000, 128
	q = make([]float32, dim)
	vs = make([]float32, n*dim)
	for i := range q {
		q[i] = rng.Float32()
	}
	for i := range vs {
		vs[i] = rng.Float32()
	}
	return
}

// BenchmarkScan measures one exact 10k x 128 scan with a K=10 bound, the
// unit of work of an unfiltered benchmark query.
func BenchmarkScan(b *testing.B) {
	q32, vs, n, dim := kernelData()
	q := widen(q32)
	b.ResetTimer()
	for it := 0; it < b.N; it++ {
		tk := &topK{k: 10, h: make(hitHeap, 0, 11)}
		for s := 0; s < n; s++ {
			bd := tk.bound()
			d := l2sqBound(q, vs[s*dim:(s+1)*dim], bd)
			if d > bd {
				continue
			}
			tk.admit(d, int64(s), 0)
		}
	}
}

// TestKernelMatchesReference checks the distance kernels against a plain
// float64 sum: equal within rounding for every vector and dimension, and the
// bounded, unbounded and encoded-bytes paths bit-identical to each other.
func TestKernelMatchesReference(t *testing.T) {
	t.Logf("useSIMD=%v", useSIMD)
	q, vs, n, dim := kernelData()
	qw := widen(q)
	for s := 0; s < n; s++ {
		v := vs[s*dim : (s+1)*dim]
		want := l2sq(q, v)
		if got := l2sqBound(qw, v, want); got != want {
			t.Fatalf("slot %d: bound kernel %v != l2sq %v", s, got, want)
		}
		var ref float64
		for i := range q {
			d := float64(q[i]) - float64(v[i])
			ref += d * d
		}
		if math.Abs(want-ref) > 1e-12*ref {
			t.Fatalf("slot %d: %v vs reference %v", s, want, ref)
		}
		enc := make([]byte, 4*dim)
		for i, f := range v {
			binary.LittleEndian.PutUint32(enc[4*i:], math.Float32bits(f))
		}
		if l2sqBytes(q, enc) != want {
			t.Fatalf("slot %d: bytes kernel differs", s)
		}
		// The bounded kernel over encoded bytes must be bit-identical to
		// the bounded kernel over a decoded slice: the TopK ordering key
		// and the printed distance of one row have to agree exactly.
		if got := l2sqBytesBound(qw, enc, math.Inf(1)); got != want {
			t.Fatalf("slot %d: bounded bytes kernel %v != %v", s, got, want)
		}
		for _, bd := range []float64{0, want * 0.5, want, want * 2} {
			if got, exp := l2sqBytesBound(qw, enc, bd), l2sqBound(qw, v, bd); got != exp {
				t.Fatalf("slot %d: bound %v: bytes %v != slice %v", s, bd, got, exp)
			}
		}
	}
	for _, d := range []int{0, 1, 3, 4, 7, 15, 16, 17, 31, 33, 100, 300} {
		a := make([]float32, d)
		b := make([]float32, d)
		for i := range a {
			a[i], b[i] = float32(i)*0.5, float32(d-i)
		}
		var ref float64
		for i := range a {
			x := float64(a[i]) - float64(b[i])
			ref += x * x
		}
		if got := l2sq(a, b); got != ref {
			t.Fatalf("dim %d: %v vs %v", d, got, ref)
		}
	}
}

// TestKernelPathsAgree compares the portable and the SIMD kernel (when the
// CPU has one) on random data of many lengths.
func TestKernelPathsAgree(t *testing.T) {
	if !useSIMD {
		t.Skip("no SIMD kernel on this CPU")
	}
	rng := rand.New(rand.NewSource(3))
	for it := 0; it < 2000; it++ {
		n := rng.Intn(300)
		a := make([]float32, n)
		b := make([]float32, n)
		for i := range a {
			a[i] = float32(rng.NormFloat64() * 10)
			b[i] = float32(rng.NormFloat64() * 10)
		}
		simd := l2sqSIMD(widen(a), b)
		useSIMD = false
		scalar := l2sq(a, b)
		useSIMD = true
		if math.Abs(simd-scalar) > 1e-12*math.Max(1, scalar) {
			t.Fatalf("n=%d simd %v scalar %v", n, simd, scalar)
		}
	}
}
