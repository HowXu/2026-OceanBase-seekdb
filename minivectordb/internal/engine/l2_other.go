//go:build !amd64

package engine

// useSIMD is a variable only so tests can toggle it on every platform.
var useSIMD = false

func l2sqSIMD(a []float64, b []float32) float64 { panic("no SIMD kernel on this platform") }

func l2sqSIMDBytes(a []float64, b []byte) float64 { panic("no SIMD kernel on this platform") }
