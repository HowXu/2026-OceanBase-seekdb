// Package storage implements the append-only record log. The log is both the
// redo log and the primary data store: row payloads are read back in place
// through a read-only mmap, so table data never has to be fully loaded.
package storage

import (
	"encoding/binary"
	"hash/crc32"
	"io"
	"os"
	"time"
)

// Record layout: [u32 payloadLen][u32 crc][u8 type][u64 txn][payload].
// The CRC (Castagnoli) covers type, txn and payload.
const HdrSize = 17

const (
	RecPut    byte = 1 // u32 tableID, i64 pk, row bytes
	RecDel    byte = 2 // u32 tableID, i64 pk
	RecCommit byte = 3
	RecAbort  byte = 4
	RecDDL    byte = 5 // JSON catalog operation, self-committing

	maxRecordLen = 1 << 30
)

var crcTable = crc32.MakeTable(crc32.Castagnoli)

// SyncMode controls what Sync waits for before COMMIT is acknowledged.
type SyncMode int

const (
	SyncData SyncMode = iota // fdatasync: survives kill -9 and power loss
	SyncNone                 // write(2) only: survives kill -9, not power loss
)

type Log struct {
	f       *os.File
	size    int64 // logical end, including buffered bytes
	flushed int64 // bytes handed to the kernel
	synced  int64
	buf     []byte
	mode    SyncMode
	m       mapping
	stats   Stats
}

// Stats counts write and sync system calls for diagnostics.
type Stats struct {
	SyncCalls    int64 `json:"sync_calls"`
	SyncNs       int64 `json:"sync_ns"`
	WriteCalls   int64 `json:"write_calls"`
	BytesWritten int64 `json:"bytes_written"`
}

func (l *Log) Stats() Stats { return l.stats }

func (l *Log) sync() error {
	t := time.Now()
	err := fdatasync(l.f)
	l.stats.SyncCalls++
	l.stats.SyncNs += int64(time.Since(t))
	return err
}

const flushThreshold = 4 << 20

func OpenLog(path string, mode SyncMode) (*Log, error) {
	f, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE, 0o644)
	if err != nil {
		return nil, err
	}
	st, err := f.Stat()
	if err != nil {
		f.Close()
		return nil, err
	}
	return &Log{f: f, size: st.Size(), flushed: st.Size(), synced: st.Size(), mode: mode,
		buf: make([]byte, 0, 64<<10)}, nil
}

func (l *Log) Size() int64 { return l.size }

// Begin starts a record; the caller appends the payload to *Buf() and calls End.
func (l *Log) Begin(typ byte, txn uint64) int64 {
	off := l.size
	var h [HdrSize]byte
	h[8] = typ
	binary.LittleEndian.PutUint64(h[9:], txn)
	l.buf = append(l.buf, h[:]...)
	return off
}

func (l *Log) Buf() *[]byte { return &l.buf }

// End seals the record started at off and returns the offset of its payload.
func (l *Log) End(off int64) (int64, error) {
	rec := l.buf[off-l.flushed:]
	n := len(rec) - HdrSize
	binary.LittleEndian.PutUint32(rec[0:], uint32(n))
	binary.LittleEndian.PutUint32(rec[4:], crc32.Checksum(rec[8:], crcTable))
	l.size += int64(len(rec))
	if len(l.buf) >= flushThreshold {
		return off + HdrSize, l.Flush()
	}
	return off + HdrSize, nil
}

// Append writes a complete record in one call.
func (l *Log) Append(typ byte, txn uint64, payload []byte) (int64, error) {
	off := l.Begin(typ, txn)
	l.buf = append(l.buf, payload...)
	return l.End(off)
}

func (l *Log) Flush() error {
	if len(l.buf) == 0 {
		return nil
	}
	if _, err := l.f.WriteAt(l.buf, l.flushed); err != nil {
		return err
	}
	l.stats.WriteCalls++
	l.stats.BytesWritten += int64(len(l.buf))
	l.flushed += int64(len(l.buf))
	l.buf = l.buf[:0]
	if cap(l.buf) > 2*flushThreshold {
		l.buf = make([]byte, 0, 64<<10)
	}
	return nil
}

// Sync makes every appended record durable according to the sync mode.
func (l *Log) Sync() error {
	if err := l.Flush(); err != nil {
		return err
	}
	if l.mode == SyncNone || l.synced == l.flushed {
		return nil
	}
	if err := l.sync(); err != nil {
		return err
	}
	l.synced = l.flushed
	return nil
}

// View returns n bytes at off without copying. The slice stays valid until
// the next Truncate or Close; callers must not modify it.
//
// After Flush and Prepare, View on ranges below the flushed end does not
// modify the Log and may be called from several goroutines until the next
// write.
func (l *Log) View(off int64, n int) []byte {
	if off >= l.flushed {
		s := off - l.flushed
		return l.buf[s : s+int64(n) : s+int64(n)]
	}
	if off+int64(n) > l.flushed {
		// The record straddles the flush boundary; push the tail to the kernel.
		if err := l.Flush(); err != nil {
			panic(err)
		}
	}
	if b := l.m.view(l.f, off, n, l.flushed); b != nil {
		return b
	}
	b := make([]byte, n)
	if _, err := l.f.ReadAt(b, off); err != nil && err != io.EOF {
		panic(err)
	}
	return b
}

// Prepare makes the mapping cover every flushed byte, so that later View
// calls on flushed ranges never remap.
func (l *Log) Prepare() {
	if l.flushed > 0 {
		l.m.view(l.f, l.flushed-1, 1, l.flushed)
	}
}

// Map returns the read-only mapping of the flushed part of the log, or nil
// when the log is not mapped. The slice holds the log bytes from offset
// zero, so a caller that knows where a record starts can read it without
// going through View. The slice stays valid until the next write.
func (l *Log) Map() []byte {
	if l.m.bad || l.flushed == 0 || int64(len(l.m.data)) < l.flushed {
		return nil
	}
	return l.m.data[:l.flushed]
}

// Truncate drops everything after n (used to cut an incomplete tail).
func (l *Log) Truncate(n int64) error {
	if err := l.Flush(); err != nil {
		return err
	}
	l.m.unmap()
	if err := l.f.Truncate(n); err != nil {
		return err
	}
	l.size, l.flushed, l.synced = n, n, n
	return l.sync()
}

func (l *Log) Close() error {
	err := l.Sync()
	l.m.unmap()
	if cerr := l.f.Close(); err == nil {
		err = cerr
	}
	return err
}
