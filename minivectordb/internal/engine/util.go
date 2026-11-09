package engine

import (
	"encoding/binary"
	"math"
)

func rowInt(b []byte, off int) int64 { return int64(binary.LittleEndian.Uint64(b[off:])) }

// l2sqBytes is l2sq against an encoded little-endian float32 vector. It uses
// the same accumulation order as l2sq so both paths give identical results.
func l2sqBytes(q []float32, b []byte) float64 {
	b = b[:4*len(q)]
	f := func(i int) float64 { return float64(math.Float32frombits(binary.LittleEndian.Uint32(b[4*i:]))) }
	var s0, s1, s2, s3 float64
	i := 0
	for ; i+4 <= len(q); i += 4 {
		d0 := float64(q[i]) - f(i)
		d1 := float64(q[i+1]) - f(i+1)
		d2 := float64(q[i+2]) - f(i+2)
		d3 := float64(q[i+3]) - f(i+3)
		s0 += d0 * d0
		s1 += d1 * d1
		s2 += d2 * d2
		s3 += d3 * d3
	}
	for ; i < len(q); i++ {
		d := float64(q[i]) - f(i)
		s0 += d * d
	}
	return (s0 + s1) + (s2 + s3)
}
