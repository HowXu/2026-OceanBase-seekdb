package engine

import (
	"encoding/binary"
	"math"
)

func rowInt(b []byte, off int) int64 { return int64(binary.LittleEndian.Uint64(b[off:])) }

// l2sqBytes is l2sq against an encoded little-endian float32 vector, with the
// same summation order as l2sqBound so both paths give identical results.
func l2sqBytes(q []float32, b []byte) float64 {
	b = b[:4*len(q)]
	var buf [256]float32
	v := buf[:0]
	if len(q) > len(buf) {
		v = make([]float32, 0, len(q))
	}
	for i := range q {
		v = append(v, math.Float32frombits(binary.LittleEndian.Uint32(b[4*i:])))
	}
	return l2sq(q, v)
}

// l2sqBytesBound is l2sqBound against an encoded little-endian float32
// vector. a is the already widened query. The result is bit-identical to the
// path that reads a decoded float32 slice, because the kernel converts the
// stored floats in the same order.
func l2sqBytesBound(a []float64, b []byte, bound float64) float64 {
	if useSIMD {
		return l2sqSIMDBytes(a, b)
	}
	var buf [256]float32
	var v []float32
	if len(a) <= len(buf) {
		v = buf[:len(a)]
	} else {
		v = make([]float32, len(a))
	}
	for i := range v {
		v[i] = math.Float32frombits(binary.LittleEndian.Uint32(b[4*i:]))
	}
	return l2sqBound(a, v, bound)
}
