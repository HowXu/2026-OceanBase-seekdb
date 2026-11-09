package engine

import (
	"math"
	"strconv"
	"strings"

	sp "minivectordb/internal/sqlparse"
	"minivectordb/internal/types"
)

// rowFn evaluates an expression against encoded row bytes (nil without FROM).
type rowFn func(b []byte) types.Value

// compiled is a type-checked expression. All type errors are reported at
// compile time, so evaluation itself never fails.
type compiled struct {
	fn    rowFn
	kind  types.Kind
	col   int // definition index when the expression is a bare column, else -1
	konst *types.Value
	// set when the expression is L2_DISTANCE(vector column, constant vector)
	l2q []float32
}

type scope struct {
	t     *Table
	alias string
}

func (sc *scope) resolve(c *sp.ColRef) (int, *types.SQLError) {
	full := c.Name
	if c.Table != "" {
		full = c.Table + "." + c.Name
	}
	if sc.t == nil {
		return -1, types.ErrBadField(full)
	}
	if c.Table != "" && !strings.EqualFold(c.Table, sc.t.Name) && !strings.EqualFold(c.Table, sc.alias) {
		return -1, types.ErrBadField(full)
	}
	i := sc.t.colIndex(c.Name)
	if i < 0 {
		return -1, types.ErrBadField(full)
	}
	return i, nil
}

func constant(v types.Value) *compiled {
	vv := v
	return &compiled{fn: func([]byte) types.Value { return vv }, kind: v.K, col: -1, konst: &vv}
}

func (sc *scope) compile(e sp.Expr) (*compiled, *types.SQLError) {
	switch x := e.(type) {
	case *sp.IntLit:
		return constant(types.Int(x.V)), nil
	case *sp.FloatLit:
		return constant(types.Float(x.V)), nil
	case *sp.StrLit:
		return constant(types.Str(x.V)), nil
	case *sp.ColRef:
		i, err := sc.resolve(x)
		if err != nil {
			return nil, err
		}
		t := sc.t
		c := t.Cols[i]
		var fn rowFn
		switch c.Type {
		case types.TBigint:
			o := t.fixOff[i]
			fn = func(b []byte) types.Value { return types.Int(rowInt(b, o)) }
		case types.TVector:
			fn = func(b []byte) types.Value { return t.col(b, i) }
		default:
			fn = func(b []byte) types.Value { return types.Str(string(t.varBytes(b, i))) }
		}
		return &compiled{fn: fn, kind: kindOf(c.Type), col: i}, nil
	case *sp.Func:
		return sc.compileFunc(x)
	case *sp.Cmp, *sp.And:
		p, err := sc.pred(e)
		if err != nil {
			return nil, err
		}
		return &compiled{fn: func(b []byte) types.Value {
			if p(b) {
				return types.Int(1)
			}
			return types.Int(0)
		}, kind: types.KInt, col: -1}, nil
	case *sp.Ordinal:
		return constant(types.Int(x.N)), nil
	}
	return nil, types.ErrUnsupported("unsupported expression")
}

func kindOf(t uint8) types.Kind {
	switch t {
	case types.TBigint:
		return types.KInt
	case types.TVector:
		return types.KVec
	}
	return types.KStr
}

// vectorArg compiles an argument that must evaluate to a vector.
func (sc *scope) vectorArg(e sp.Expr, fname string) (*compiled, *types.SQLError) {
	c, err := sc.compile(e)
	if err != nil {
		return nil, err
	}
	switch {
	case c.kind == types.KVec:
		return c, nil
	case c.konst != nil && c.kind == types.KStr:
		v, verr := types.ParseVector(c.konst.S)
		if verr != nil {
			return nil, verr
		}
		return constant(types.Vec(v)), nil
	}
	return nil, types.ErrTypeMismatch("Incorrect arguments to " + fname + ": expected a vector")
}

func (sc *scope) compileFunc(f *sp.Func) (*compiled, *types.SQLError) {
	switch f.Name {
	case "VEC_FROM_TEXT", "STRING_TO_VECTOR", "TO_VECTOR", "VECTOR":
		if len(f.Args) != 1 {
			return nil, types.ErrWrongArgs(f.Name)
		}
		return sc.vectorArg(f.Args[0], f.Name)
	case "L2_DISTANCE":
		if len(f.Args) != 2 {
			return nil, types.ErrWrongArgs(f.Name)
		}
		a, err := sc.vectorArg(f.Args[0], f.Name)
		if err != nil {
			return nil, err
		}
		b, err := sc.vectorArg(f.Args[1], f.Name)
		if err != nil {
			return nil, err
		}
		if a.konst == nil && b.konst != nil {
			// keep the column on the left
		} else if a.konst != nil && b.konst == nil {
			a, b = b, a
		}
		if a.konst != nil && b.konst != nil {
			if len(a.konst.V) != len(b.konst.V) {
				return nil, types.ErrVectorDim(len(a.konst.V), len(b.konst.V))
			}
			return constant(types.Float(math.Sqrt(l2sq(a.konst.V, b.konst.V)))), nil
		}
		if b.konst != nil {
			q := b.konst.V
			if a.col >= 0 && len(q) != sc.t.Dim {
				return nil, types.ErrVectorDim(sc.t.Dim, len(q))
			}
			off := sc.t.vecOff
			if a.col >= 0 {
				return &compiled{fn: func(b []byte) types.Value {
					return types.Float(math.Sqrt(l2sqBytes(q, b[off:])))
				}, kind: types.KFloat, col: -1, l2q: q}, nil
			}
			af := a.fn
			return &compiled{fn: func(b []byte) types.Value {
				return types.Float(math.Sqrt(l2sq(q, af(b).V)))
			}, kind: types.KFloat, col: -1}, nil
		}
		af, bf := a.fn, b.fn
		return &compiled{fn: func(r []byte) types.Value {
			return types.Float(math.Sqrt(l2sq(af(r).V, bf(r).V)))
		}, kind: types.KFloat, col: -1}, nil
	}
	return nil, types.ErrUnsupported("function " + f.Name + " is not supported")
}

// pred compiles a WHERE condition.
func (sc *scope) pred(e sp.Expr) (func([]byte) bool, *types.SQLError) {
	switch x := e.(type) {
	case *sp.And:
		l, err := sc.pred(x.L)
		if err != nil {
			return nil, err
		}
		r, err := sc.pred(x.R)
		if err != nil {
			return nil, err
		}
		return func(b []byte) bool { return l(b) && r(b) }, nil
	case *sp.Cmp:
		l, err := sc.compile(x.L)
		if err != nil {
			return nil, err
		}
		r, err := sc.compile(x.R)
		if err != nil {
			return nil, err
		}
		return cmpPred(x.Op, l, r)
	}
	return nil, types.ErrUnsupported("WHERE condition must be a comparison or AND of comparisons")
}

func opTest(op string) func(c int) bool {
	switch op {
	case "=":
		return func(c int) bool { return c == 0 }
	case "<>":
		return func(c int) bool { return c != 0 }
	case "<":
		return func(c int) bool { return c < 0 }
	case "<=":
		return func(c int) bool { return c <= 0 }
	case ">":
		return func(c int) bool { return c > 0 }
	}
	return func(c int) bool { return c >= 0 }
}

func cmpPred(op string, l, r *compiled) (func([]byte) bool, *types.SQLError) {
	if l.kind == types.KVec || r.kind == types.KVec {
		return nil, types.ErrTypeMismatch("Vectors cannot be compared with " + op)
	}
	// A numeric side against a string constant: compare numerically when the
	// string is a valid number, otherwise reject instead of guessing.
	if l.kind == types.KStr && r.kind != types.KStr {
		if err := numifyConst(l); err != nil {
			return nil, err
		}
	} else if r.kind == types.KStr && l.kind != types.KStr {
		if err := numifyConst(r); err != nil {
			return nil, err
		}
	}
	test := opTest(op)
	lf, rf := l.fn, r.fn
	if l.kind == types.KStr && r.kind == types.KStr {
		return func(b []byte) bool { return test(strings.Compare(lf(b).S, rf(b).S)) }, nil
	}
	if l.kind == types.KInt && r.kind == types.KInt {
		if r.konst != nil {
			k := r.konst.I
			return func(b []byte) bool { return test(cmpInt(lf(b).I, k)) }, nil
		}
		return func(b []byte) bool { return test(cmpInt(lf(b).I, rf(b).I)) }, nil
	}
	return func(b []byte) bool { return test(cmpNum(lf(b), rf(b))) }, nil
}

func numifyConst(c *compiled) *types.SQLError {
	if c.konst == nil {
		return types.ErrTypeMismatch("Cannot compare a VARCHAR column with a number")
	}
	s := strings.TrimSpace(c.konst.S)
	if i, err := strconv.ParseInt(s, 10, 64); err == nil {
		*c = *constant(types.Int(i))
		return nil
	}
	if f, err := strconv.ParseFloat(s, 64); err == nil && !math.IsNaN(f) && !math.IsInf(f, 0) {
		*c = *constant(types.Float(f))
		return nil
	}
	return types.ErrTypeMismatch("Cannot compare '" + c.konst.S + "' with a number")
}

func cmpInt(a, b int64) int {
	switch {
	case a < b:
		return -1
	case a > b:
		return 1
	}
	return 0
}

// cmpNum compares int/float values exactly, including large int64 values.
func cmpNum(a, b types.Value) int {
	if a.K == types.KInt && b.K == types.KInt {
		return cmpInt(a.I, b.I)
	}
	if a.K == types.KInt {
		return -cmpFloatInt(b.F, a.I)
	}
	if b.K == types.KInt {
		return cmpFloatInt(a.F, b.I)
	}
	switch {
	case a.F < b.F:
		return -1
	case a.F > b.F:
		return 1
	}
	return 0
}

func cmpFloatInt(f float64, i int64) int {
	if f < -9.3e18 {
		return -1
	}
	if f > 9.3e18 {
		return 1
	}
	fl := math.Floor(f)
	fi := int64(fl)
	if fi != i {
		return cmpInt(fi, i)
	}
	if f > fl {
		return 1
	}
	return 0
}

// compareValues orders two values of the same static kind.
func compareValues(a, b types.Value) int {
	if a.K == types.KStr {
		return strings.Compare(a.S, b.S)
	}
	return cmpNum(a, b)
}
