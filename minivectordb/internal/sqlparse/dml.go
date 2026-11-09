package sqlparse

import (
	"math"
	"strconv"
	"strings"
)

func parseInt(s string) (int64, error) { return strconv.ParseInt(s, 10, 64) }

func (ps *parser) insert() (Stmt, error) {
	ps.p++
	if ps.isKw("ignore") || ps.isKw("low_priority") || ps.isKw("delayed") || ps.isKw("high_priority") {
		return nil, unsupported("INSERT %s is not supported", strings.ToUpper(ps.cur().Text))
	}
	if err := ps.expectKw("into"); err != nil {
		return nil, err
	}
	tbl, err := ps.tableName()
	if err != nil {
		return nil, err
	}
	ins := &Insert{Table: tbl}
	if ps.acceptOp("(") {
		ins.Cols = []string{}
		for {
			c, err := ps.ident()
			if err != nil {
				return nil, err
			}
			ins.Cols = append(ins.Cols, c)
			if !ps.acceptOp(",") {
				break
			}
		}
		if err := ps.expectOp(")"); err != nil {
			return nil, err
		}
	}
	if ps.isKw("select") || ps.isKw("set") {
		return nil, unsupported("INSERT ... %s is not supported", strings.ToUpper(ps.cur().Text))
	}
	if !ps.acceptKw("values") && !ps.acceptKw("value") {
		return nil, ps.errNear()
	}
	for {
		if err := ps.expectOp("("); err != nil {
			return nil, err
		}
		var row []Expr
		if !ps.isOp(")") {
			for {
				e, err := ps.expr()
				if err != nil {
					return nil, err
				}
				row = append(row, e)
				if !ps.acceptOp(",") {
					break
				}
			}
		}
		if err := ps.expectOp(")"); err != nil {
			return nil, err
		}
		ins.Rows = append(ins.Rows, row)
		if !ps.acceptOp(",") {
			break
		}
	}
	if ps.isKw("on") {
		return nil, unsupported("ON DUPLICATE KEY UPDATE is not supported")
	}
	return ins, nil
}

func (ps *parser) selectStmt() (Stmt, error) {
	ps.p++
	if ps.isKw("distinct") {
		return nil, unsupported("SELECT DISTINCT is not supported")
	}
	sel := &Select{Limit: -1}
	for {
		var it SelItem
		start := ps.cur().Pos
		if ps.acceptOp("*") {
			it.Star = true
		} else if ps.cur().Kind == Ident && ps.peek(1).Kind == Op && ps.peek(1).Text == "." &&
			ps.peek(2).Kind == Op && ps.peek(2).Text == "*" {
			ps.p += 3
			it.Star = true
		} else {
			e, err := ps.expr()
			if err != nil {
				return nil, err
			}
			it.Expr = e
			it.Text = strings.TrimSpace(ps.src[start:ps.toks[ps.p-1].End])
			if ps.acceptKw("as") {
				t := ps.cur()
				if t.Kind == String {
					ps.p++
					it.Alias = t.Text
				} else if it.Alias, err = ps.ident(); err != nil {
					return nil, err
				}
			} else if t := ps.cur(); t.Kind == QIdent || t.Kind == String ||
				(t.Kind == Ident && !reserved[strings.ToLower(t.Text)]) {
				ps.p++
				it.Alias = t.Text
			}
		}
		sel.Items = append(sel.Items, it)
		if !ps.acceptOp(",") {
			break
		}
	}
	if ps.acceptKw("from") {
		tbl, err := ps.tableName()
		if err != nil {
			return nil, err
		}
		sel.Table = tbl
		if ps.acceptKw("as") {
			if sel.Alias, err = ps.ident(); err != nil {
				return nil, err
			}
		} else if t := ps.cur(); t.Kind == QIdent || (t.Kind == Ident && !reserved[strings.ToLower(t.Text)]) {
			ps.p++
			sel.Alias = t.Text
		}
		if ps.isOp(",") || ps.isKw("join") || ps.isKw("inner") || ps.isKw("left") ||
			ps.isKw("right") || ps.isKw("cross") || ps.isKw("natural") {
			return nil, unsupported("JOIN is not supported")
		}
	}
	if ps.acceptKw("where") {
		w, err := ps.expr()
		if err != nil {
			return nil, err
		}
		sel.Where = w
	}
	if ps.isKw("group") || ps.isKw("having") {
		return nil, unsupported("GROUP BY / HAVING is not supported")
	}
	if ps.acceptKw("order") {
		if err := ps.expectKw("by"); err != nil {
			return nil, err
		}
		for {
			var oi OrderItem
			if t := ps.cur(); t.Kind == Number && !strings.ContainsAny(t.Text, ".eE") {
				ps.p++
				n, err := parseInt(t.Text)
				if err != nil {
					return nil, ps.errNear()
				}
				oi.Expr = &Ordinal{N: n}
			} else {
				e, err := ps.expr()
				if err != nil {
					return nil, err
				}
				oi.Expr = e
			}
			if ps.acceptKw("desc") {
				oi.Desc = true
			} else {
				ps.acceptKw("asc")
			}
			sel.Order = append(sel.Order, oi)
			if !ps.acceptOp(",") {
				break
			}
		}
		if ps.acceptKw("approximate") || ps.acceptKw("approx") {
			sel.Approx = true
		}
	}
	if ps.acceptKw("limit") {
		a, err := ps.limitNum()
		if err != nil {
			return nil, err
		}
		if ps.acceptOp(",") {
			b, err := ps.limitNum()
			if err != nil {
				return nil, err
			}
			sel.Offset, sel.Limit = a, b
		} else {
			sel.Limit = a
			if ps.acceptKw("offset") {
				if sel.Offset, err = ps.limitNum(); err != nil {
					return nil, err
				}
			}
		}
	}
	if ps.isKw("union") || ps.isKw("for") || ps.isKw("into") {
		return nil, unsupported("%s is not supported", strings.ToUpper(ps.cur().Text))
	}
	return sel, nil
}

func (ps *parser) limitNum() (int64, error) {
	t := ps.cur()
	if t.Kind != Number || strings.ContainsAny(t.Text, ".eE") {
		return 0, ps.errNear()
	}
	ps.p++
	n, err := strconv.ParseUint(t.Text, 10, 64)
	if err != nil || n > math.MaxInt64 {
		return math.MaxInt64, nil
	}
	return int64(n), nil
}

func (ps *parser) update() (Stmt, error) {
	ps.p++
	tbl, err := ps.tableName()
	if err != nil {
		return nil, err
	}
	if ps.isOp(",") || ps.isKw("join") {
		return nil, unsupported("multi-table UPDATE is not supported")
	}
	if err := ps.expectKw("set"); err != nil {
		return nil, err
	}
	up := &Update{Table: tbl}
	for {
		c, err := ps.ident()
		if err != nil {
			return nil, err
		}
		if ps.acceptOp(".") {
			if c, err = ps.ident(); err != nil {
				return nil, err
			}
		}
		if err := ps.expectOp("="); err != nil {
			return nil, err
		}
		e, err := ps.expr()
		if err != nil {
			return nil, err
		}
		up.Sets = append(up.Sets, Assign{Col: c, Expr: e})
		if !ps.acceptOp(",") {
			break
		}
	}
	if ps.acceptKw("where") {
		if up.Where, err = ps.expr(); err != nil {
			return nil, err
		}
	}
	if ps.isKw("order") || ps.isKw("limit") {
		return nil, unsupported("UPDATE with ORDER BY / LIMIT is not supported")
	}
	return up, nil
}

func (ps *parser) delete() (Stmt, error) {
	ps.p++
	if err := ps.expectKw("from"); err != nil {
		return nil, err
	}
	tbl, err := ps.tableName()
	if err != nil {
		return nil, err
	}
	del := &Delete{Table: tbl}
	if ps.acceptKw("where") {
		if del.Where, err = ps.expr(); err != nil {
			return nil, err
		}
	}
	if ps.isKw("order") || ps.isKw("limit") || ps.isKw("using") {
		return nil, unsupported("DELETE with ORDER BY / LIMIT / USING is not supported")
	}
	return del, nil
}

func (ps *parser) set() (Stmt, error) {
	ps.p++
	ps.acceptKw("session")
	if ps.isOp("@") {
		return nil, unsupported("user variables are not supported")
	}
	name, err := ps.ident()
	if err != nil {
		return nil, err
	}
	for ps.acceptOp(".") {
		n2, err := ps.ident()
		if err != nil {
			return nil, err
		}
		name = name + "." + n2
	}
	if !ps.acceptOp("=") {
		if err := ps.expectKw("to"); err != nil {
			return nil, err
		}
	}
	var v Expr
	if t := ps.cur(); t.Kind == Ident || t.Kind == QIdent {
		ps.p++
		v = &StrLit{V: t.Text}
	} else if v, err = ps.primary(); err != nil {
		return nil, err
	}
	return &Set{Name: strings.ToLower(name), Value: v}, nil
}

// Expressions: AND-chains of comparisons over primaries.

func (ps *parser) expr() (Expr, error) {
	l, err := ps.cmp()
	if err != nil {
		return nil, err
	}
	for ps.isKw("and") || ps.isOp("&&") {
		ps.p++
		r, err := ps.cmp()
		if err != nil {
			return nil, err
		}
		l = &And{L: l, R: r}
	}
	if ps.isKw("or") || ps.isOp("||") || ps.isKw("xor") {
		return nil, unsupported("OR / XOR conditions are not supported")
	}
	return l, nil
}

var cmpOps = map[string]string{"=": "=", "<": "<", "<=": "<=", ">": ">", ">=": ">=", "<>": "<>", "!=": "<>"}

func (ps *parser) cmp() (Expr, error) {
	if ps.isKw("not") || ps.isOp("!") {
		return nil, unsupported("NOT is not supported")
	}
	l, err := ps.primary()
	if err != nil {
		return nil, err
	}
	t := ps.cur()
	if t.Kind == Op {
		if op, ok := cmpOps[t.Text]; ok {
			ps.p++
			r, err := ps.primary()
			if err != nil {
				return nil, err
			}
			if nt := ps.cur(); nt.Kind == Op {
				if _, ok := cmpOps[nt.Text]; ok {
					return nil, unsupported("chained comparisons are not supported")
				}
			}
			return &Cmp{Op: op, L: l, R: r}, nil
		}
		switch t.Text {
		case "+", "-", "*", "/", "%", "<=>", "<->":
			return nil, unsupported("operator '%s' is not supported", t.Text)
		}
	}
	if t.Kind == Ident {
		switch strings.ToLower(t.Text) {
		case "is", "in", "like", "between", "not", "regexp", "rlike", "div", "mod":
			return nil, unsupported("operator '%s' is not supported", strings.ToUpper(t.Text))
		}
	}
	return l, nil
}

func (ps *parser) primary() (Expr, error) {
	t := ps.cur()
	switch t.Kind {
	case Number:
		ps.p++
		return numLit(t.Text, false)
	case String:
		ps.p++
		return &StrLit{V: t.Text}, nil
	case Op:
		switch t.Text {
		case "-", "+":
			nt := ps.peek(1)
			if nt.Kind == Number {
				ps.p += 2
				return numLit(nt.Text, t.Text == "-")
			}
			return nil, unsupported("arithmetic expressions are not supported")
		case "(":
			ps.p++
			if ps.isKw("select") {
				return nil, unsupported("subqueries are not supported")
			}
			e, err := ps.expr()
			if err != nil {
				return nil, err
			}
			if ps.isOp(",") {
				return nil, unsupported("row constructors are not supported")
			}
			if err := ps.expectOp(")"); err != nil {
				return nil, err
			}
			return e, nil
		case "[":
			return ps.bracketVector()
		}
		return nil, ps.errNear()
	case Ident, QIdent:
		low := strings.ToLower(t.Text)
		if t.Kind == Ident {
			if low == "null" {
				return nil, unsupported("NULL is not supported")
			}
			if low == "true" || low == "false" {
				return nil, unsupported("boolean literals are not supported")
			}
			if low == "select" || low == "exists" {
				return nil, unsupported("subqueries are not supported")
			}
			if low == "case" || low == "interval" || low == "binary" {
				return nil, unsupported("%s expressions are not supported", strings.ToUpper(low))
			}
		}
		if t.Kind == Ident && ps.peek(1).Kind == Op && ps.peek(1).Text == "(" {
			return ps.funcCall()
		}
		name, err := ps.ident()
		if err != nil {
			return nil, err
		}
		if ps.acceptOp(".") {
			col, err := ps.ident()
			if err != nil {
				return nil, err
			}
			return &ColRef{Table: name, Name: col}, nil
		}
		return &ColRef{Name: name}, nil
	}
	return nil, ps.errNear()
}

func numLit(text string, neg bool) (Expr, error) {
	if neg {
		text = "-" + text
	}
	if !strings.ContainsAny(text, ".eE") {
		if v, err := parseInt(text); err == nil {
			return &IntLit{V: v}, nil
		}
	}
	f, err := strconv.ParseFloat(text, 64)
	if err != nil && !math.IsInf(f, 0) {
		return nil, syntaxErr("invalid number '%s'", text)
	}
	return &FloatLit{V: f}, nil
}

// bracketVector parses an unquoted [a, b, ...] literal into its string form.
func (ps *parser) bracketVector() (Expr, error) {
	start := ps.cur().Pos
	ps.p++
	for !ps.isOp("]") {
		if ps.cur().Kind == EOF {
			return nil, ps.errNear()
		}
		ps.p++
	}
	end := ps.cur().End
	ps.p++
	return &StrLit{V: ps.src[start:end]}, nil
}

func (ps *parser) funcCall() (Expr, error) {
	name := strings.ToUpper(ps.next().Text)
	ps.p++ // (
	f := &Func{Name: name}
	if !ps.isOp(")") {
		for {
			if ps.isOp("*") {
				return nil, unsupported("aggregate functions are not supported")
			}
			e, err := ps.expr()
			if err != nil {
				return nil, err
			}
			f.Args = append(f.Args, e)
			if !ps.acceptOp(",") {
				break
			}
		}
	}
	if err := ps.expectOp(")"); err != nil {
		return nil, err
	}
	switch name {
	case "L2_DISTANCE", "VEC_FROM_TEXT", "STRING_TO_VECTOR", "TO_VECTOR", "VECTOR":
		return f, nil
	case "COUNT", "SUM", "AVG", "MIN", "MAX", "GROUP_CONCAT":
		return nil, unsupported("aggregate function %s is not supported", name)
	}
	return nil, unsupported("function %s is not supported", name)
}
