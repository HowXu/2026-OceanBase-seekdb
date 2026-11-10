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

// cgroupCPUs returns the CPU quota of the container rounded up to whole
// CPUs (cgroup v2 cpu.max, then v1 cfs quota/period), or 0 if unlimited.
// Go 1.22 does not read the quota, so GOMAXPROCS would otherwise default to
// the host's core count.
func cgroupCPUs() int {
	quota, period := int64(-1), int64(0)
	if b, err := os.ReadFile("/sys/fs/cgroup/cpu.max"); err == nil {
		f := strings.Fields(string(b))
		if len(f) == 2 && f[0] != "max" {
			quota, _ = strconv.ParseInt(f[0], 10, 64)
			period, _ = strconv.ParseInt(f[1], 10, 64)
		}
	} else {
		q, err1 := os.ReadFile("/sys/fs/cgroup/cpu/cpu.cfs_quota_us")
		p, err2 := os.ReadFile("/sys/fs/cgroup/cpu/cpu.cfs_period_us")
		if err1 == nil && err2 == nil {
			quota, _ = strconv.ParseInt(strings.TrimSpace(string(q)), 10, 64)
			period, _ = strconv.ParseInt(strings.TrimSpace(string(p)), 10, 64)
		}
	}
	if quota <= 0 || period <= 0 {
		return 0
	}
	n := int((quota + period - 1) / period)
	if n < 1 {
		n = 1
	}
	return n
}
