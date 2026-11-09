package types

import "fmt"

// SQLError carries a MySQL-compatible error number and SQLSTATE.
type SQLError struct {
	Name  string // semantic error name used by the test driver, e.g. duplicate_key
	Code  int
	State string
	Msg   string
}

func (e *SQLError) Error() string { return fmt.Sprintf("ERROR %d (%s): %s", e.Code, e.State, e.Msg) }

func newErr(code int, state, format string, a ...any) *SQLError {
	return &SQLError{Name: names[code], Code: code, State: state, Msg: fmt.Sprintf(format, a...)}
}

func named(name string, e *SQLError) *SQLError { e.Name = name; return e }

var names = map[int]string{
	1064: "syntax_error",
	1235: "unsupported_sql",
	1050: "table_exists",
	1146: "table_not_found",
	1054: "column_not_found",
	1060: "duplicate_column",
	1062: "duplicate_key",
	1136: "column_count_mismatch",
	1406: "data_too_long",
	1264: "out_of_range",
	1366: "invalid_value",
	1173: "primary_key_required",
	1068: "multiple_primary_key",
	1364: "missing_value",
	1582: "wrong_arguments",
	1210: "type_mismatch",
	1061: "duplicate_index",
	1105: "internal_error",
}

func ErrSyntax(msg string) *SQLError      { return newErr(1064, "42000", "%s", msg) }
func ErrUnsupported(msg string) *SQLError { return newErr(1235, "42000", "%s", msg) }
func ErrTableExists(t string) *SQLError   { return newErr(1050, "42S01", "Table '%s' already exists", t) }
func ErrNoTable(t string) *SQLError       { return newErr(1146, "42S02", "Table '%s' doesn't exist", t) }
func ErrBadField(c string) *SQLError      { return newErr(1054, "42S22", "Unknown column '%s'", c) }
func ErrDupField(c string) *SQLError      { return newErr(1060, "42S21", "Duplicate column name '%s'", c) }
func ErrDupKey(k int64) *SQLError {
	return newErr(1062, "23000", "Duplicate entry '%d' for key 'PRIMARY'", k)
}
func ErrColCount() *SQLError {
	return newErr(1136, "21S01", "Column count doesn't match value count")
}
func ErrDataTooLong(c string) *SQLError {
	return newErr(1406, "22001", "Data too long for column '%s'", c)
}
func ErrOutOfRange(c string) *SQLError {
	return newErr(1264, "22003", "Out of range value for column '%s'", c)
}
func ErrBadValue(typ, v, c string) *SQLError {
	return newErr(1366, "HY000", "Incorrect %s value: '%s' for column '%s'", typ, v, c)
}
func ErrNoPK() *SQLError { return newErr(1173, "42000", "This table type requires a primary key") }
func ErrMultiPK() *SQLError {
	return newErr(1068, "42000", "Multiple primary key defined")
}
func ErrBadPKType(c string) *SQLError {
	return named("invalid_definition", newErr(1235, "42000", "Primary key column '%s' must be BIGINT", c))
}
func ErrMultiVector() *SQLError {
	return named("invalid_definition", newErr(1235, "42000", "Only one VECTOR column per table is supported"))
}
func ErrNoDefault(c string) *SQLError {
	return newErr(1364, "HY000", "Field '%s' doesn't have a default value", c)
}
func ErrPKUpdate() *SQLError {
	return newErr(1235, "42000", "Updating the primary key is not supported")
}
func ErrVectorFormat(s string) *SQLError {
	if len(s) > 64 {
		s = s[:64] + "..."
	}
	return named("invalid_vector", newErr(1366, "HY000", "Invalid vector literal '%s'", s))
}
func ErrVectorValue(s string) *SQLError {
	return named("invalid_vector", newErr(1366, "HY000", "Vector element '%s' is not a finite float32", s))
}
func ErrVectorDim(want, got int) *SQLError {
	return named("dimension_mismatch", newErr(1366, "HY000", "Vector dimension mismatch: expected %d, got %d", want, got))
}
func ErrNestedTxn() *SQLError {
	return named("transaction_error", newErr(1235, "25000", "Nested transactions are not supported"))
}
func ErrWrongArgs(f string) *SQLError {
	return newErr(1582, "42000", "Incorrect parameter count in the call to native function '%s'", f)
}
func ErrTypeMismatch(msg string) *SQLError { return newErr(1210, "HY000", "%s", msg) }
func ErrIndexExists(n string) *SQLError    { return newErr(1061, "42000", "Duplicate key name '%s'", n) }
func ErrNotVectorCol(c string) *SQLError {
	return named("type_mismatch", newErr(1235, "42000", "Column '%s' is not a VECTOR column", c))
}
func ErrAmbiguousOrder(c string) *SQLError {
	return newErr(1054, "42S22", "Unknown column '%s' in 'order clause'", c)
}
func ErrInternal(err error) *SQLError { return newErr(1105, "HY000", "Internal error: %v", err) }
