//go:build linux || darwin

package storage

import (
	"os"
	"syscall"
)

// mapping is a read-only shared mapping of the log file, grown in large steps.
// Pages past the flushed end are never touched.
type mapping struct {
	data []byte
	bad  bool
}

const mapStep = 1 << 30

func (m *mapping) view(f *os.File, off int64, n int, flushed int64) []byte {
	if m.bad {
		return nil
	}
	end := off + int64(n)
	if end > int64(len(m.data)) {
		m.unmap()
		size := (flushed/mapStep + 1) * mapStep
		d, err := syscall.Mmap(int(f.Fd()), 0, int(size), syscall.PROT_READ, syscall.MAP_SHARED)
		if err != nil {
			m.bad = true
			return nil
		}
		m.data = d
	}
	return m.data[off:end:end]
}

func (m *mapping) unmap() {
	if m.data != nil {
		_ = syscall.Munmap(m.data)
		m.data = nil
	}
}
