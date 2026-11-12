#include "textflag.h"

// func cpuid(eax, ecx uint32) (a, b, c, d uint32)
TEXT ·cpuid(SB), NOSPLIT, $0-24
	MOVL eax+0(FP), AX
	MOVL ecx+4(FP), CX
	CPUID
	MOVL AX, a+8(FP)
	MOVL BX, b+12(FP)
	MOVL CX, c+16(FP)
	MOVL DX, d+20(FP)
	RET

// func xgetbv0() uint32
TEXT ·xgetbv0(SB), NOSPLIT, $0-4
	MOVL $0, CX
	XGETBV
	MOVL AX, ret+0(FP)
	RET

// func l2sqBlocksAVX2(a *float64, b *byte, blocks int) float64
//
// Sum over blocks of 16 elements of (b[i]-a[i])^2 in float64, where b points
// at little-endian float32 values. Element i of
// a block goes to lane i%4 of accumulator (i/4)%4; the result is
// ((acc0+acc1)+(acc2+acc3)) reduced as (lane0+lane2)+(lane1+lane3).
TEXT ·l2sqBlocksAVX2(SB), NOSPLIT, $0-32
	MOVQ a+0(FP), SI
	MOVQ b+8(FP), DI
	MOVQ blocks+16(FP), CX
	VXORPD Y0, Y0, Y0
	VXORPD Y1, Y1, Y1
	VXORPD Y2, Y2, Y2
	VXORPD Y3, Y3, Y3
	TESTQ CX, CX
	JZ done
loop:
	VCVTPS2PD 0(DI), Y4
	VCVTPS2PD 16(DI), Y5
	VCVTPS2PD 32(DI), Y6
	VCVTPS2PD 48(DI), Y7
	VSUBPD 0(SI), Y4, Y4
	VSUBPD 32(SI), Y5, Y5
	VSUBPD 64(SI), Y6, Y6
	VSUBPD 96(SI), Y7, Y7
	VFMADD231PD Y4, Y4, Y0
	VFMADD231PD Y5, Y5, Y1
	VFMADD231PD Y6, Y6, Y2
	VFMADD231PD Y7, Y7, Y3
	ADDQ $128, SI
	ADDQ $64, DI
	DECQ CX
	JNZ loop
done:
	VADDPD Y1, Y0, Y0
	VADDPD Y3, Y2, Y2
	VADDPD Y2, Y0, Y0
	VEXTRACTF128 $1, Y0, X1
	VADDPD X1, X0, X0
	VHADDPD X0, X0, X0
	VZEROUPPER
	MOVSD X0, ret+24(FP)
	RET
