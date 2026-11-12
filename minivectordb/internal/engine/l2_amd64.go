package engine

import (
	"encoding/binary"
	"math"
	"os"
	"unsafe"
)

func cpuid(eax, ecx uint32) (a, b, c, d uint32)
func xgetbv0() uint32

// l2sqBlocksAVX2 sums the squared distance over blocks of 16 elements.
// The second operand points at the stored little-endian float32 values;
// it is declared as a byte pointer so the kernel can also be fed the
// encoded vector straight out of the log, without a decode step.
//
//go:noescape
func l2sqBlocksAVX2(a *float64, b *byte, blocks int) float64

// hasAVX2FMA reports whether the CPU and OS support AVX2 and FMA, so the
// vector kernel can be used. MVDB_NO_SIMD=1 forces the portable kernel.
func hasAVX2FMA() bool {
	if os.Getenv("MVDB_NO_SIMD") != "" {
		return false
	}
	max, _, _, _ := cpuid(0, 0)
	if max < 7 {
		return false
	}
	_, _, c1, _ := cpuid(1, 0)
	const fma, osxsave, avx = 1 << 12, 1 << 27, 1 << 28
	if c1&fma == 0 || c1&osxsave == 0 || c1&avx == 0 {
		return false
	}
	if xgetbv0()&6 != 6 { // XMM and YMM state enabled by the OS
		return false
	}
	_, b7, _, _ := cpuid(7, 0)
	return b7&(1<<5) != 0
}

var useSIMD = hasAVX2FMA()

// l2sqSIMD is the AVX2 kernel for whole 16-element blocks plus a scalar
// tail added in order.
func l2sqSIMD(a []float64, b []float32) float64 {
	n := len(a)
	blocks := n / 16
	var s float64
	if blocks > 0 {
		// The kernel reads the stored float32 values as bytes; the two
		// views describe the same little-endian words.
		s = l2sqBlocksAVX2(&a[0], (*byte)(unsafe.Pointer(&b[0])), blocks)
	}
	for i := blocks * 16; i < n; i++ {
		d := a[i] - float64(b[i])
		s += d * d
	}
	return s
}

// l2sqSIMDBytes is l2sqSIMD against an encoded little-endian float32 vector.
// The kernel converts the stored floats itself, so the encoded bytes are read
// straight from the log with no decode step and no copy.
func l2sqSIMDBytes(a []float64, b []byte) float64 {
	n := len(a)
	blocks := n / 16
	var s float64
	if blocks > 0 {
		s = l2sqBlocksAVX2(&a[0], &b[0], blocks)
	}
	for i := blocks * 16; i < n; i++ {
		d := a[i] - float64(math.Float32frombits(binary.LittleEndian.Uint32(b[4*i:])))
		s += d * d
	}
	return s
}
