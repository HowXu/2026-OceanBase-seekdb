package storage

import (
	"encoding/binary"
	"hash/crc32"
	"io"
	"os"
)

// Record is one decoded log record produced by Scanner.
type Record struct {
	Off     int64 // offset of the record header
	Type    byte
	Txn     uint64
	Payload []byte // valid until the next call to Next
}

// Scanner reads records sequentially. It stops at the first incomplete or
// corrupt record; Good is the offset just past the last valid record.
type Scanner struct {
	f    *os.File
	off  int64 // file offset of buf[0]
	buf  []byte
	pos  int
	end  int64
	Good int64
	Err  error
}

func NewScanner(l *Log, from int64) *Scanner {
	if err := l.Flush(); err != nil {
		return &Scanner{Err: err}
	}
	return &Scanner{f: l.f, off: from, end: l.flushed, Good: from}
}

func (s *Scanner) fill(need int) bool {
	if len(s.buf)-s.pos >= need {
		return true
	}
	s.off += int64(s.pos)
	rest := copy(s.buf, s.buf[s.pos:])
	s.buf = s.buf[:rest]
	s.pos = 0
	want := 1 << 20
	if need > want {
		want = need
	}
	if cap(s.buf) < want {
		nb := make([]byte, rest, want)
		copy(nb, s.buf)
		s.buf = nb
	}
	toRead := int64(cap(s.buf) - rest)
	if avail := s.end - (s.off + int64(rest)); toRead > avail {
		toRead = avail
	}
	if toRead > 0 {
		n, err := s.f.ReadAt(s.buf[rest:rest+int(toRead)], s.off+int64(rest))
		s.buf = s.buf[:rest+n]
		if err != nil && err != io.EOF {
			s.Err = err
		}
	}
	return len(s.buf)-s.pos >= need
}

func (s *Scanner) Next(r *Record) bool {
	if s.Err != nil || !s.fill(HdrSize) {
		return false
	}
	n := int(binary.LittleEndian.Uint32(s.buf[s.pos:]))
	if n > maxRecordLen || !s.fill(HdrSize+n) {
		return false
	}
	rec := s.buf[s.pos : s.pos+HdrSize+n]
	if crc32.Checksum(rec[8:], crcTable) != binary.LittleEndian.Uint32(rec[4:]) {
		return false
	}
	r.Off = s.off + int64(s.pos)
	r.Type = rec[8]
	r.Txn = binary.LittleEndian.Uint64(rec[9:])
	r.Payload = rec[HdrSize:]
	s.pos += HdrSize + n
	s.Good = s.off + int64(s.pos)
	return true
}
