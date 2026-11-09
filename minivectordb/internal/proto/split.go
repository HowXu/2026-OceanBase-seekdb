// Package proto implements statement framing on stdin and result encoding on stdout.
package proto

import (
	"bufio"
	"io"
	"strings"
)

// Splitter yields complete statements from stdin.
//
// A statement ends at a semicolon outside quotes and comments. A newline also
// ends it when the text so far is complete: no open quote, comment or
// parenthesis, and it does not end with a word or comma that needs a
// continuation. This accepts both the one-statement-per-line .test format
// and multi-line statements terminated by semicolons. A line that is a driver directive ("--restart", "--mode ann")
// is returned as its own statement.
type Splitter struct {
	r       *bufio.Reader
	pending string // unread remainder of a line after a semicolon
}

func (s *Splitter) readLine() (string, error) {
	if s.pending != "" {
		l := s.pending
		s.pending = ""
		return l, nil
	}
	return s.r.ReadString('\n')
}

// nextLineContinues peeks at already-buffered input (never blocking) and
// reports whether the next line continues the current statement, as in
// "SELECT a\nFROM t". When nothing is buffered the driver is waiting for a
// reply, so the statement must be complete.
func (s *Splitter) nextLineContinues() bool {
	var next string
	if s.pending != "" {
		next = s.pending
	} else if n := s.r.Buffered(); n > 0 {
		b, _ := s.r.Peek(n)
		next = string(b)
	} else {
		return false
	}
	next = strings.TrimLeft(next, " \t\r\n")
	if next == "" {
		return false
	}
	if strings.IndexByte(",)=<>+*/.]", next[0]) >= 0 {
		return true
	}
	j := 0
	for j < len(next) && isWord(next[j]) {
		j++
	}
	return leading[strings.ToLower(next[:j])]
}

var leading = map[string]bool{
	"from": true, "where": true, "and": true, "or": true, "order": true, "limit": true,
	"offset": true, "values": true, "value": true, "set": true, "on": true, "as": true,
	"asc": true, "desc": true, "by": true, "primary": true, "not": true, "approximate": true,
	"group": true, "having": true, "join": true, "inner": true, "left": true, "union": true,
}

func NewSplitter(r io.Reader) *Splitter {
	return &Splitter{r: bufio.NewReaderSize(r, 1<<20)}
}

// IsDirective reports whether s is a driver directive line.
func IsDirective(s string) bool {
	return len(s) > 2 && strings.HasPrefix(s, "--") && s[2] != ' ' && s[2] != '\t' && s[2] != '-'
}

// Next returns the next statement without its terminating semicolon. ok is
// false at end of input.
func (s *Splitter) Next() (stmt string, ok bool) {
	var sb strings.Builder
	var quote byte
	inBlock := false
	depth := 0
	for {
		line, err := s.readLine()
		if line == "" && err != nil {
			st := strings.TrimSpace(sb.String())
			return st, st != ""
		}
		if quote == 0 && !inBlock && strings.TrimSpace(sb.String()) == "" {
			if t := strings.TrimSpace(line); IsDirective(t) {
				return t, true
			}
		}
		i := 0
		for i < len(line) {
			c := line[i]
			switch {
			case inBlock:
				if c == '*' && i+1 < len(line) && line[i+1] == '/' {
					inBlock = false
					i++
				}
			case quote != 0:
				sb.WriteByte(c)
				if c == '\\' && quote != '`' && i+1 < len(line) {
					i++
					sb.WriteByte(line[i])
				} else if c == quote {
					quote = 0
				}
			case c == '\'' || c == '"' || c == '`':
				quote = c
				sb.WriteByte(c)
			case c == '#' || (c == '-' && strings.HasPrefix(line[i:], "-- ")) ||
				(c == '-' && strings.TrimRight(line[i:], "\r\n") == "--"):
				i = len(line) // rest of the line is a comment
				continue
			case c == '(' || c == '[':
				depth++
				sb.WriteByte(c)
			case c == ')' || c == ']':
				depth--
				sb.WriteByte(c)
			case c == '/' && i+1 < len(line) && line[i+1] == '*':
				inBlock = true
				i++
			case c == ';':
				st := strings.TrimSpace(sb.String())
				if st != "" {
					// Push back the remainder of the line for the next call.
					if rest := line[i+1:]; strings.TrimSpace(rest) != "" {
						s.pending = rest
					}
					return st, true
				}
				sb.Reset()
			default:
				sb.WriteByte(c)
			}
			i++
		}
		if quote == 0 && !inBlock && depth <= 0 {
			st := strings.TrimSpace(sb.String())
			if st == "" {
				sb.Reset()
			} else if complete(st) && !s.nextLineContinues() {
				return st, true
			} else {
				sb.WriteByte('\n')
			}
		} else {
			sb.WriteByte('\n')
		}
		if err != nil {
			st := strings.TrimSpace(sb.String())
			return st, st != ""
		}
	}
}

var continuation = map[string]bool{
	"select": true, "from": true, "where": true, "and": true, "or": true, "by": true,
	"order": true, "limit": true, "values": true, "value": true, "set": true, "into": true,
	"on": true, "as": true, "table": true, "index": true, "update": true, "delete": true,
	"insert": true, "create": true, "vector": true, "not": true, "if": true, "exists": true,
	"primary": true, "key": true, "offset": true, "distinct": true,
}

// complete reports whether st can end here without a semicolon.
func complete(st string) bool {
	last := st[len(st)-1]
	if strings.IndexByte(",=<>+-*/(.", last) >= 0 {
		return false
	}
	j := len(st)
	for j > 0 && (isWord(st[j-1])) {
		j--
	}
	return !continuation[strings.ToLower(st[j:])]
}

func isWord(c byte) bool {
	return c == '_' || c >= '0' && c <= '9' || c|0x20 >= 'a' && c|0x20 <= 'z'
}
