package engine

import (
	"errors"
	"strings"

	sp "minivectordb/internal/sqlparse"
	"minivectordb/internal/types"
)

// Result is the outcome of one statement.
type Result struct {
	IsQuery  bool
	Columns  []string
	Kinds    []types.Kind
	Rows     [][]types.Value
	Affected int64
	// HasAffected is set by INSERT, UPDATE and DELETE so the response carries
	// affected_rows; other statements reply with a bare ok.
	HasAffected bool
}

// ErrQuit is returned when the client asked to end the session.
var ErrQuit = errors.New("quit")

// Exec parses and runs one statement.
func (db *DB) Exec(sql string) (*Result, error) {
	st, err := sp.Parse(sql)
	if err != nil {
		var pe *sp.Error
		if errors.As(err, &pe) {
			if pe.Unsupported {
				return nil, types.ErrUnsupported(pe.Msg)
			}
			return nil, types.ErrSyntax(pe.Msg)
		}
		return nil, types.ErrSyntax(err.Error())
	}
	res, rerr := db.run(st)
	// Guard against a nil *SQLError stored in the error interface.
	if se, ok := rerr.(*types.SQLError); ok && se == nil {
		rerr = nil
	}
	if rerr != nil {
		return nil, rerr
	}
	return res, nil
}

func (db *DB) run(st sp.Stmt) (*Result, error) {
	switch s := st.(type) {
	case *sp.Select:
		return db.execSelect(s)
	case *sp.Insert:
		return db.execInsert(s)
	case *sp.Update:
		return db.execUpdate(s)
	case *sp.Delete:
		return db.execDelete(s)
	case *sp.CreateTable:
		return db.execCreate(s)
	case *sp.CreateIndex:
		return db.execCreateIndex(s)
	case *sp.DropTable:
		return db.execDrop(s)
	case *sp.Begin:
		if db.txn != nil {
			return nil, types.ErrNestedTxn()
		}
		db.txn = db.newTxn()
		return &Result{}, nil
	case *sp.Commit:
		if db.txn != nil {
			if err := db.commit(); err != nil {
				return nil, ioErr(err)
			}
		}
		return &Result{}, nil
	case *sp.Rollback:
		if db.txn != nil {
			db.rollback()
		}
		return &Result{}, nil
	case *sp.Set:
		return db.execSet(s)
	case *sp.Quit:
		return nil, ErrQuit
	}
	return nil, types.ErrUnsupported("statement not supported")
}

// ioErr wraps a storage failure; the process cannot guarantee durability
// after this, so callers should treat it as fatal.
func ioErr(err error) error { return &FatalError{Err: err} }

type FatalError struct{ Err error }

func (e *FatalError) Error() string { return "fatal storage error: " + e.Err.Error() }

func (db *DB) execSet(s *sp.Set) (*Result, error) {
	name := s.Name
	if i := strings.LastIndexByte(name, '.'); i >= 0 {
		name = name[i+1:]
	}
	switch name {
	case "search_mode", "vector_search_mode", "mvdb_search_mode", "query_mode", "vector_mode":
		v, ok := s.Value.(*sp.StrLit)
		if !ok {
			return nil, types.ErrTypeMismatch("search mode must be 'exact' or 'ann'")
		}
		m := strings.ToLower(v.V)
		switch m {
		case "exact", "ann":
			db.opts.SearchMode = m
		case "approx", "approximate":
			db.opts.SearchMode = "ann"
		default:
			return nil, types.ErrTypeMismatch("search mode must be 'exact' or 'ann'")
		}
		return &Result{}, nil
	case "autocommit", "names", "character_set_client", "character_set_results", "sql_mode",
		"time_zone", "character_set_connection", "collation_connection":
		if name == "autocommit" {
			if v, ok := s.Value.(*sp.IntLit); ok && v.V == 0 {
				return nil, types.ErrUnsupported("autocommit=0 is not supported; use BEGIN")
			}
		}
		return &Result{}, nil
	}
	return nil, types.ErrUnsupported("unknown system variable '" + s.Name + "'")
}

func (db *DB) table(name string) (*Table, *types.SQLError) {
	t := db.tables[strings.ToLower(name)]
	if t == nil {
		return nil, types.ErrNoTable(name)
	}
	return t, nil
}

func (db *DB) execCreate(s *sp.CreateTable) (*Result, error) {
	if _, ok := db.tables[strings.ToLower(s.Name)]; ok {
		if s.IfNotExists {
			return &Result{}, nil
		}
		return nil, types.ErrTableExists(s.Name)
	}
	cols := make([]types.Column, len(s.Cols))
	pk, nvec := -1, 0
	seen := map[string]bool{}
	for i, c := range s.Cols {
		low := strings.ToLower(c.Name)
		if seen[low] {
			return nil, types.ErrDupField(c.Name)
		}
		seen[low] = true
		cols[i] = types.Column{Name: c.Name, Type: uint8(c.Type), Param: c.Param}
		if c.Type == sp.TVector {
			nvec++
			if c.Param > 65535 {
				return nil, types.ErrUnsupported("vector dimension too large")
			}
		}
		if c.Type == sp.TVarchar && c.Param > 65535 {
			return nil, types.ErrUnsupported("VARCHAR length too large")
		}
		if c.PK {
			if pk >= 0 {
				return nil, types.ErrMultiPK()
			}
			if c.Type != sp.TBigint {
				return nil, types.ErrBadPKType(c.Name)
			}
			pk = i
		}
	}
	if pk < 0 {
		return nil, types.ErrNoPK()
	}
	if nvec > 1 {
		return nil, types.ErrMultiVector()
	}
	d := &ddl{Op: "create_table", ID: db.nextTID, Name: s.Name, Cols: cols, PK: pk}
	if err := db.writeDDL(d); err != nil {
		return nil, ioErr(err)
	}
	return &Result{}, nil
}

func (db *DB) execDrop(s *sp.DropTable) (*Result, error) {
	t := db.tables[strings.ToLower(s.Name)]
	if t == nil {
		if s.IfExists {
			return &Result{}, nil
		}
		return nil, types.ErrNoTable(s.Name)
	}
	if err := db.writeDDL(&ddl{Op: "drop_table", ID: t.ID}); err != nil {
		return nil, ioErr(err)
	}
	return &Result{}, nil
}

func (db *DB) execCreateIndex(s *sp.CreateIndex) (*Result, error) {
	t, err := db.table(s.Table)
	if err != nil {
		return nil, err
	}
	ci := t.colIndex(s.Column)
	if ci < 0 {
		return nil, types.ErrBadField(s.Column)
	}
	if t.Cols[ci].Type != types.TVector {
		return nil, types.ErrNotVectorCol(s.Column)
	}
	if t.Index != nil {
		if s.IfNotExists {
			return &Result{}, nil
		}
		return nil, types.ErrIndexExists(s.Name)
	}
	if err := db.writeDDL(&ddl{Op: "create_index", ID: t.ID, Index: s.Name, Column: t.Cols[ci].Name}); err != nil {
		return nil, ioErr(err)
	}
	t.Index.rebuild(db, t)
	return &Result{}, nil
}
