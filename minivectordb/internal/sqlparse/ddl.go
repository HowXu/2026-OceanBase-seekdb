package sqlparse

import "strings"

func (ps *parser) ifNotExists() (bool, error) {
	if ps.acceptKw("if") {
		if err := ps.expectKw("not"); err != nil {
			return false, err
		}
		if err := ps.expectKw("exists"); err != nil {
			return false, err
		}
		return true, nil
	}
	return false, nil
}

func (ps *parser) create() (Stmt, error) {
	ps.p++ // CREATE
	switch {
	case ps.acceptKw("table"):
		return ps.createTable()
	case ps.isKw("vector"):
		ps.p++
		if err := ps.expectKw("index"); err != nil {
			return nil, err
		}
		return ps.createIndex()
	case ps.isKw("index"), ps.isKw("unique"), ps.isKw("fulltext"), ps.isKw("spatial"):
		return nil, unsupported("only CREATE VECTOR INDEX is supported")
	case ps.isKw("database"), ps.isKw("schema"), ps.isKw("view"), ps.isKw("temporary"):
		return nil, unsupported("CREATE %s is not supported", strings.ToUpper(ps.cur().Text))
	}
	return nil, ps.errNear()
}

func (ps *parser) createIndex() (Stmt, error) {
	ine, err := ps.ifNotExists()
	if err != nil {
		return nil, err
	}
	name, err := ps.ident()
	if err != nil {
		return nil, err
	}
	if err := ps.expectKw("on"); err != nil {
		return nil, err
	}
	tbl, err := ps.tableName()
	if err != nil {
		return nil, err
	}
	if err := ps.expectOp("("); err != nil {
		return nil, err
	}
	col, err := ps.ident()
	if err != nil {
		return nil, err
	}
	if ps.isOp(",") {
		return nil, unsupported("vector index on multiple columns is not supported")
	}
	if err := ps.expectOp(")"); err != nil {
		return nil, err
	}
	// Optional index options such as WITH (distance=l2, type=hnsw) are accepted and ignored.
	if ps.acceptKw("with") {
		if err := ps.skipParens(); err != nil {
			return nil, err
		}
	}
	return &CreateIndex{Name: name, Table: tbl, Column: col, IfNotExists: ine}, nil
}

func (ps *parser) skipParens() error {
	if err := ps.expectOp("("); err != nil {
		return err
	}
	depth := 1
	for depth > 0 {
		t := ps.next()
		switch {
		case t.Kind == EOF:
			return syntaxErr("syntax error at end of input")
		case t.Kind == Op && t.Text == "(":
			depth++
		case t.Kind == Op && t.Text == ")":
			depth--
		}
	}
	return nil
}

func (ps *parser) typeParam() (int, error) {
	if err := ps.expectOp("("); err != nil {
		return 0, err
	}
	t := ps.cur()
	if t.Kind != Number || strings.ContainsAny(t.Text, ".eE") {
		return 0, ps.errNear()
	}
	ps.p++
	n, err := parseInt(t.Text)
	if err != nil || n <= 0 || n > 1<<20 {
		return 0, &Error{Msg: "invalid type length '" + t.Text + "'"}
	}
	if err := ps.expectOp(")"); err != nil {
		return 0, err
	}
	return int(n), nil
}

func (ps *parser) createTable() (Stmt, error) {
	ine, err := ps.ifNotExists()
	if err != nil {
		return nil, err
	}
	name, err := ps.tableName()
	if err != nil {
		return nil, err
	}
	if err := ps.expectOp("("); err != nil {
		return nil, err
	}
	ct := &CreateTable{Name: name, IfNotExists: ine}
	var tablePK []string
	for {
		if ps.isKw("primary") {
			ps.p++
			if err := ps.expectKw("key"); err != nil {
				return nil, err
			}
			if err := ps.expectOp("("); err != nil {
				return nil, err
			}
			for {
				c, err := ps.ident()
				if err != nil {
					return nil, err
				}
				tablePK = append(tablePK, c)
				if !ps.acceptOp(",") {
					break
				}
			}
			if err := ps.expectOp(")"); err != nil {
				return nil, err
			}
		} else if ps.isKw("index") || ps.isKw("key") || ps.isKw("unique") || ps.isKw("foreign") ||
			ps.isKw("constraint") || ps.isKw("vector") && ps.peek(1).Kind == Ident {
			return nil, unsupported("table constraints other than PRIMARY KEY are not supported")
		} else {
			cd, err := ps.colDef()
			if err != nil {
				return nil, err
			}
			ct.Cols = append(ct.Cols, cd)
		}
		if ps.acceptOp(",") {
			continue
		}
		if err := ps.expectOp(")"); err != nil {
			return nil, err
		}
		break
	}
	// Trailing table options (ENGINE=..., CHARSET=...) are not part of the subset.
	if ps.cur().Kind != EOF && !ps.isOp(";") {
		return nil, unsupported("table options are not supported")
	}
	if len(tablePK) > 1 {
		return nil, unsupported("composite primary keys are not supported")
	}
	if len(tablePK) == 1 {
		found := false
		for i := range ct.Cols {
			if strings.EqualFold(ct.Cols[i].Name, tablePK[0]) {
				if ct.Cols[i].PK {
					return nil, &Error{Msg: "multiple primary key defined"}
				}
				ct.Cols[i].PK = true
				found = true
			}
		}
		if !found {
			return nil, &Error{Msg: "key column '" + tablePK[0] + "' doesn't exist in table"}
		}
	}
	return ct, nil
}

func (ps *parser) colDef() (ColDef, error) {
	var cd ColDef
	name, err := ps.ident()
	if err != nil {
		return cd, err
	}
	cd.Name = name
	t := ps.cur()
	if t.Kind != Ident {
		return cd, ps.errNear()
	}
	ps.p++
	switch strings.ToLower(t.Text) {
	case "bigint":
		cd.Type = TBigint
		if ps.isOp("(") { // display width such as BIGINT(20)
			if _, err := ps.typeParam(); err != nil {
				return cd, err
			}
		}
	case "varchar":
		cd.Type = TVarchar
		if cd.Param, err = ps.typeParam(); err != nil {
			return cd, err
		}
	case "vector":
		cd.Type = TVector
		if cd.Param, err = ps.typeParam(); err != nil {
			return cd, err
		}
	case "int", "integer", "smallint", "tinyint", "mediumint", "float", "double", "decimal",
		"char", "text", "blob", "date", "datetime", "timestamp", "bool", "boolean", "json",
		"real", "numeric", "time", "year", "binary", "varbinary", "longtext", "mediumtext", "enum", "bit":
		return cd, unsupported("column type '%s' is not supported", strings.ToUpper(t.Text))
	default:
		return cd, syntaxErr("syntax error near '%s'", snippet(ps.src, t.Pos))
	}
	for {
		switch {
		case ps.isKw("primary"):
			ps.p++
			if err := ps.expectKw("key"); err != nil {
				return cd, err
			}
			cd.PK = true
		case ps.isKw("not"):
			ps.p++
			if err := ps.expectKw("null"); err != nil {
				return cd, err
			}
		case ps.isKw("null"), ps.isKw("default"), ps.isKw("unique"), ps.isKw("auto_increment"),
			ps.isKw("references"), ps.isKw("check"), ps.isKw("comment"), ps.isKw("unsigned"),
			ps.isKw("collate"), ps.isKw("character"), ps.isKw("generated"):
			return cd, unsupported("column attribute '%s' is not supported", strings.ToUpper(ps.cur().Text))
		default:
			return cd, nil
		}
	}
}

func (ps *parser) drop() (Stmt, error) {
	ps.p++
	if !ps.acceptKw("table") {
		return nil, unsupported("only DROP TABLE is supported")
	}
	d := &DropTable{}
	if ps.acceptKw("if") {
		if err := ps.expectKw("exists"); err != nil {
			return nil, err
		}
		d.IfExists = true
	}
	name, err := ps.tableName()
	if err != nil {
		return nil, err
	}
	if ps.isOp(",") {
		return nil, unsupported("dropping multiple tables is not supported")
	}
	d.Name = name
	return d, nil
}
