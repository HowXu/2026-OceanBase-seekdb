package sqlparse

type ColType int

const (
	TBigint ColType = iota + 1
	TVarchar
	TVector
)

type Stmt interface{ stmt() }

type ColDef struct {
	Name  string
	Type  ColType
	Param int // VARCHAR length or VECTOR dimension
	PK    bool
}

type CreateTable struct {
	Name        string
	IfNotExists bool
	Cols        []ColDef
}

type CreateIndex struct {
	Name        string
	Table       string
	Column      string
	IfNotExists bool
}

type DropTable struct {
	Name     string
	IfExists bool
}

type Insert struct {
	Table string
	Cols  []string // nil means all columns in table order
	Rows  [][]Expr
}

type SelItem struct {
	Star  bool
	Expr  Expr
	Alias string
	Text  string // source text, used as the column name without an alias
}

type OrderItem struct {
	Expr Expr
	Desc bool
}

type Select struct {
	Table  string // empty when there is no FROM clause
	Alias  string
	Items  []SelItem
	Where  Expr
	Order  []OrderItem
	Approx bool  // ORDER BY ... APPROXIMATE
	Limit  int64 // -1 means no limit
	Offset int64
}

type Assign struct {
	Col  string
	Expr Expr
}

type Update struct {
	Table string
	Sets  []Assign
	Where Expr
}

type Delete struct {
	Table string
	Where Expr
}

type Begin struct{}
type Commit struct{}
type Rollback struct{}
type Quit struct{}

type Set struct {
	Name  string
	Value Expr
}

func (*CreateTable) stmt() {}
func (*CreateIndex) stmt() {}
func (*DropTable) stmt()   {}
func (*Insert) stmt()      {}
func (*Select) stmt()      {}
func (*Update) stmt()      {}
func (*Delete) stmt()      {}
func (*Begin) stmt()       {}
func (*Commit) stmt()      {}
func (*Rollback) stmt()    {}
func (*Quit) stmt()        {}
func (*Set) stmt()         {}

type Expr interface{ expr() }

type ColRef struct {
	Table string
	Name  string
}

type IntLit struct{ V int64 }
type FloatLit struct{ V float64 }
type StrLit struct{ V string }

// Ordinal is a bare integer in ORDER BY that refers to a select item position.
type Ordinal struct{ N int64 }

type Cmp struct {
	Op   string // = < <= > >= <>
	L, R Expr
}

type And struct{ L, R Expr }

type Func struct {
	Name string // upper-cased
	Args []Expr
}

func (*ColRef) expr()   {}
func (*IntLit) expr()   {}
func (*FloatLit) expr() {}
func (*StrLit) expr()   {}
func (*Ordinal) expr()  {}
func (*Cmp) expr()      {}
func (*And) expr()      {}
func (*Func) expr()     {}
