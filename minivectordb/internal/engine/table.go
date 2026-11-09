// Package engine implements the catalog, transactions, recovery and query
// execution on top of the storage log.
package engine

import (
	"encoding/binary"
	"math"
	"strings"

	"minivectordb/internal/types"
)

// Table rows are stored in a fixed storage order: the BIGINT primary key,
// the other BIGINT columns, the vector column, then VARCHAR columns. Every
// column except VARCHARs therefore sits at a constant byte offset.
type Table struct {
	ID      uint32
	Name    string
	Cols    []types.Column // definition order
	PK      int
	Vec     int // definition index of the vector column, -1 if none
	Dim     int
	order   []int // storage position -> definition index
	fixOff  []int // definition index -> byte offset, -1 for VARCHAR
	varIdx  []int // definition index -> ordinal among VARCHARs, -1 otherwise
	varBase int   // offset of the first VARCHAR
	vecOff  int

	rows  map[int64]int64 // committed rows: pk -> log offset of the row bytes
	Index *VecIndex
}

func newTable(id uint32, name string, cols []types.Column, pk int) *Table {
	t := &Table{ID: id, Name: name, Cols: cols, PK: pk, Vec: -1, rows: make(map[int64]int64)}
	t.order = append(t.order, pk)
	for i, c := range cols {
		if c.Type == types.TBigint && i != pk {
			t.order = append(t.order, i)
		}
	}
	for i, c := range cols {
		if c.Type == types.TVector {
			t.order = append(t.order, i)
			t.Vec, t.Dim = i, c.Param
		}
	}
	for i, c := range cols {
		if c.Type == types.TVarchar {
			t.order = append(t.order, i)
		}
	}
	t.fixOff = make([]int, len(cols))
	t.varIdx = make([]int, len(cols))
	off, nv := 0, 0
	for _, di := range t.order {
		c := cols[di]
		t.varIdx[di] = -1
		switch c.Type {
		case types.TBigint:
			t.fixOff[di] = off
			off += 8
		case types.TVector:
			t.fixOff[di] = off
			t.vecOff = off
			off += 4 * c.Param
		default:
			if nv == 0 {
				t.varBase = off
			}
			t.fixOff[di] = -1
			t.varIdx[di] = nv
			nv++
		}
	}
	if nv == 0 {
		t.varBase = off
	}
	return t
}

func (t *Table) colIndex(name string) int {
	for i, c := range t.Cols {
		if strings.EqualFold(c.Name, name) {
			return i
		}
	}
	return -1
}

// encode appends the storage encoding of a definition-order row.
func (t *Table) encode(dst []byte, row []types.Value) []byte {
	for _, di := range t.order {
		v := row[di]
		switch t.Cols[di].Type {
		case types.TBigint:
			dst = binary.LittleEndian.AppendUint64(dst, uint64(v.I))
		case types.TVector:
			for _, f := range v.V {
				dst = binary.LittleEndian.AppendUint32(dst, math.Float32bits(f))
			}
		default:
			dst = binary.AppendUvarint(dst, uint64(len(v.S)))
			dst = append(dst, v.S...)
		}
	}
	return dst
}

func rowPK(b []byte) int64 { return int64(binary.LittleEndian.Uint64(b)) }

// col decodes definition column di from encoded row bytes.
func (t *Table) col(b []byte, di int) types.Value {
	c := t.Cols[di]
	switch c.Type {
	case types.TBigint:
		return types.Int(int64(binary.LittleEndian.Uint64(b[t.fixOff[di]:])))
	case types.TVector:
		v := make([]float32, c.Param)
		types.DecodeVecInto(v, b[t.vecOff:])
		return types.Vec(v)
	}
	return types.Str(string(t.varBytes(b, di)))
}

func (t *Table) varBytes(b []byte, di int) []byte {
	p := t.varBase
	for k := t.varIdx[di]; ; k-- {
		l, n := binary.Uvarint(b[p:])
		if k == 0 {
			return b[p+n : p+n+int(l)]
		}
		p += n + int(l)
	}
}

func (t *Table) decode(b []byte) []types.Value {
	out := make([]types.Value, len(t.Cols))
	for i := range t.Cols {
		out[i] = t.col(b, i)
	}
	return out
}
