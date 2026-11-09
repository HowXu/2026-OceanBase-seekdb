package main

import (
	"os"
	"strconv"
	"strings"
)

// cgroupMemLimit returns the container memory limit in bytes, or 0 if none
// is set. cgroup v2 is checked first, then v1.
func cgroupMemLimit() int64 {
	for _, p := range []string{"/sys/fs/cgroup/memory.max", "/sys/fs/cgroup/memory/memory.limit_in_bytes"} {
		b, err := os.ReadFile(p)
		if err != nil {
			continue
		}
		s := strings.TrimSpace(string(b))
		if s == "max" {
			return 0
		}
		n, err := strconv.ParseInt(s, 10, 64)
		if err != nil || n <= 0 || n >= 1<<60 {
			return 0
		}
		return n
	}
	return 0
}
