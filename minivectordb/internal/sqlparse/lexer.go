// Package sqlparse implements the lexer and parser for the MiniVectorDB SQL subset.
package sqlparse

import (
	"fmt"
	"strings"
)

type Kind int

const (
	EOF Kind = iota
	Ident
	QIdent
	Number
	String
	Op
)

type Token struct {
	Kind Kind
	Text string // identifier, unescaped string, number text or operator
	Pos  int    // byte offset of the token start
	End  int    // byte offset just past the token
}

// Error is a parse failure. Unsupported marks valid-looking SQL outside the subset.
type Error struct {
	Unsupported bool
	Msg         string
}

func (e *Error) Error() string { return e.Msg }

func syntaxErr(format string, a ...any) *Error {
	return &Error{Msg: fmt.Sprintf(format, a...)}
}

func unsupported(format string, a ...any) *Error {
	return &Error{Unsupported: true, Msg: fmt.Sprintf(format, a...)}
}

func isSpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'
}
func isDigit(c byte) bool      { return c >= '0' && c <= '9' }
func isIdentStart(c byte) bool { return c == '_' || c >= 0x80 || (c|0x20 >= 'a' && c|0x20 <= 'z') }
func isIdentPart(c byte) bool  { return isIdentStart(c) || isDigit(c) || c == '$' }

var multiOps = []string{"<->", "<=>", "<=", ">=", "<>", "!=", "&&", "||"}

// Lex splits src into tokens. Comments are skipped.
func Lex(src string) ([]Token, error) {
	var toks []Token
	n := len(src)
	i := 0
	for i < n {
		c := src[i]
		switch {
		case isSpace(c):
			i++
		case c == '-' && i+1 < n && src[i+1] == '-' && (i+2 >= n || isSpace(src[i+2])):
			for i < n && src[i] != '\n' {
				i++
			}
		case c == '#':
			for i < n && src[i] != '\n' {
				i++
			}
		case c == '/' && i+1 < n && src[i+1] == '*':
			j := strings.Index(src[i+2:], "*/")
			if j < 0 {
				return nil, syntaxErr("unterminated comment")
			}
			i += j + 4
		case isDigit(c) || (c == '.' && i+1 < n && isDigit(src[i+1])):
			j := scanNumber(src, i)
			toks = append(toks, Token{Number, src[i:j], i, j})
			i = j
		case isIdentStart(c):
			j := i + 1
			for j < n && isIdentPart(src[j]) {
				j++
			}
			toks = append(toks, Token{Ident, src[i:j], i, j})
			i = j
		case c == '`':
			var sb strings.Builder
			j := i + 1
			for {
				if j >= n {
					return nil, syntaxErr("unterminated quoted identifier")
				}
				if src[j] == '`' {
					if j+1 < n && src[j+1] == '`' {
						sb.WriteByte('`')
						j += 2
						continue
					}
					j++
					break
				}
				sb.WriteByte(src[j])
				j++
			}
			toks = append(toks, Token{QIdent, sb.String(), i, j})
			i = j
		case c == '\'' || c == '"':
			s, j, err := scanString(src, i)
			if err != nil {
				return nil, err
			}
			toks = append(toks, Token{String, s, i, j})
			i = j
		default:
			op := ""
			for _, m := range multiOps {
				if strings.HasPrefix(src[i:], m) {
					op = m
					break
				}
			}
			if op == "" && strings.IndexByte("(),;*=<>+-/%.[]!", c) >= 0 {
				op = string(c)
			}
			if op == "" {
				return nil, syntaxErr("syntax error near %q", snippet(src, i))
			}
			toks = append(toks, Token{Op, op, i, i + len(op)})
			i += len(op)
		}
	}
	toks = append(toks, Token{EOF, "", n, n})
	return toks, nil
}

func snippet(src string, i int) string {
	end := i + 20
	if end > len(src) {
		end = len(src)
	}
	return src[i:end]
}

func scanNumber(src string, i int) int {
	n := len(src)
	j := i
	for j < n && isDigit(src[j]) {
		j++
	}
	if j < n && src[j] == '.' {
		j++
		for j < n && isDigit(src[j]) {
			j++
		}
	}
	if j < n && (src[j] == 'e' || src[j] == 'E') {
		k := j + 1
		if k < n && (src[k] == '+' || src[k] == '-') {
			k++
		}
		if k < n && isDigit(src[k]) {
			for k < n && isDigit(src[k]) {
				k++
			}
			j = k
		}
	}
	return j
}

// scanString reads a quoted literal with MySQL escapes and doubled quotes.
func scanString(src string, i int) (string, int, error) {
	q := src[i]
	// Fast path: no escapes or doubled quotes before the closing quote, so
	// the literal is a substring of src and needs no copy.
	//
	// The closing quote is located with strings.IndexByte, which is
	// vectorised, and the escape check is a second vectorised scan over the
	// same range. A byte-at-a-time loop costs one cycle per character, which
	// dominates the insert path: a row carries a whole vector literal.
	rest := src[i+1:]
	if j := strings.IndexByte(rest, q); j >= 0 {
		if strings.IndexByte(rest[:j], '\\') < 0 &&
			!(j+1 < len(rest) && rest[j+1] == q) {
			return rest[:j], i + 1 + j + 1, nil
		}
	}
	var sb strings.Builder
	j := i + 1
	n := len(src)
	for {
		if j >= n {
			return "", 0, syntaxErr("unterminated string literal")
		}
		c := src[j]
		if c == q {
			if j+1 < n && src[j+1] == q {
				sb.WriteByte(q)
				j += 2
				continue
			}
			return sb.String(), j + 1, nil
		}
		if c == '\\' && j+1 < n {
			e := src[j+1]
			switch e {
			case 'n':
				sb.WriteByte('\n')
			case 't':
				sb.WriteByte('\t')
			case 'r':
				sb.WriteByte('\r')
			case 'b':
				sb.WriteByte('\b')
			case '0':
				sb.WriteByte(0)
			case 'Z':
				sb.WriteByte(26)
			case '%', '_':
				sb.WriteByte('\\')
				sb.WriteByte(e)
			default:
				sb.WriteByte(e)
			}
			j += 2
			continue
		}
		sb.WriteByte(c)
		j++
	}
}
