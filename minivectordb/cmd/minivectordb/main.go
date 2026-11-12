// Command minivectordb runs the database as a foreground process speaking the
// JSONL protocol of STDIO_PROTOCOL.md: after recovery it prints
// {"type":"ready"}, then answers each request line on stdin with exactly one
// response line on stdout. Logs go to stderr only.
package main

import (
	"bufio"
	"bytes"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"runtime"
	"runtime/debug"
	"strconv"
	"strings"
	"syscall"

	"minivectordb/internal/engine"
	"minivectordb/internal/proto"
	"minivectordb/internal/storage"
	"minivectordb/internal/types"
)

func main() {
	dataDir := flag.String("data-dir", envOr("MVDB_DATA_DIR", "/data"), "data directory")
	mode := flag.String("mode", envOr("MVDB_SEARCH_MODE", "exact"), "default vector search mode when a request has no search_mode: exact or ann")
	sync := flag.String("sync", envOr("MVDB_SYNC", "fdatasync"), "commit durability: fdatasync or none")
	indexMB := flag.Int64("index-mem-mb", envInt("MVDB_INDEX_MEM_MB", 256), "memory budget for in-memory vectors per table")
	flag.Parse()

	logf := func(format string, a ...any) { fmt.Fprintf(os.Stderr, "minivectordb: "+format+"\n", a...) }
	defaultMode := strings.ToLower(*mode)
	opts := engine.Options{SearchMode: defaultMode, Logf: logf, IndexMemBytes: *indexMB << 20}
	if lim := cgroupMemLimit(); lim > 0 {
		// memory.peak includes page cache, so keep the heap well below the
		// limit: vectors get at most a quarter, and the GC targets 60%.
		if q := lim / 4; q < opts.IndexMemBytes {
			opts.IndexMemBytes = q
		}
		debug.SetMemoryLimit(lim * 6 / 10)
		logf("cgroup memory limit %d MiB, vector budget %d MiB", lim>>20, opts.IndexMemBytes>>20)
	}
	if n := cgroupCPUs(); n > 0 && n < runtime.GOMAXPROCS(0) && os.Getenv("GOMAXPROCS") == "" {
		runtime.GOMAXPROCS(n)
		logf("cgroup CPU quota %d, GOMAXPROCS %d", n, n)
	}
	// Peak memory is scored. The live heap is small (vector copy plus
	// indexes), and most garbage comes from parsing large INSERT statements,
	// so a tighter GC target lowers the peak without a measurable time cost.
	// The live set holds no pointers (float32 vectors and integer maps), so
	// marking is cheap and a low target does not thrash the collector.
	if os.Getenv("GOGC") == "" {
		debug.SetGCPercent(20)
	}
	switch *sync {
	case "none":
		opts.Sync = storage.SyncNone
	default:
		opts.Sync = storage.SyncData
	}
	if defaultMode != "exact" && defaultMode != "ann" {
		logf("invalid --mode %q", *mode)
		os.Exit(2)
	}
	db, err := engine.Open(*dataDir, opts)
	if err != nil {
		logf("open %s: %v", *dataDir, err)
		os.Exit(1)
	}

	out := bufio.NewWriterSize(os.Stdout, 1<<16)
	// SIGTERM/SIGINT are a normal shutdown: roll back and close. Requests run
	// on the main goroutine, so the signal is handled between requests.
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGTERM, syscall.SIGINT)

	type line struct {
		b   []byte
		eof bool
	}
	// Requests are read on the main goroutine, one at a time, so a request
	// is handled without a goroutine hand-off. SIGTERM/SIGINT are checked
	// between requests; a signal that arrives while blocked on stdin is
	// handled by a watcher that closes stdin, which ends the read with EOF.
	rd := bufio.NewReaderSize(os.Stdin, 1<<20)
	next := func() line {
		// ReadSlice hands back a view into the reader buffer instead of
		// allocating a fresh slice per request. The request is fully
		// processed before the next read, and proto.ParseRequest copies
		// everything it keeps, so the view never outlives the buffer.
		b, err := rd.ReadSlice('\n')
		if err == bufio.ErrBufferFull {
			// Line longer than the buffer: ReadSlice has already consumed
			// the first len(b) bytes, so copy them and append the rest.
			head := append([]byte(nil), b...)
			var rest []byte
			rest, err = rd.ReadBytes('\n')
			b = append(head, rest...)
		}
		if len(b) == 0 && err != nil {
			if err != io.EOF && !errors.Is(err, os.ErrClosed) {
				logf("read stdin: %v", err)
			}
			return line{eof: true}
		}
		return line{b: b}
	}
	stop := make(chan struct{})
	go func() {
		<-sigs
		close(stop)
		os.Stdin.Close()
	}()

	// Recovery and index rebuild are done inside engine.Open, so the
	// handshake is sent only now, before the first request is read.
	out.WriteString(proto.Ready)
	out.Flush()

	closeDB := func() int {
		if err := db.Close(); err != nil {
			logf("close: %v", err)
			return 1
		}
		return 0
	}
	enc := &proto.Encoder{}
	for {
		select {
		case <-stop:
			os.Exit(closeDB())
		default:
		}
		l := next()
		if l.eof {
			os.Exit(closeDB())
		}
		if len(bytes.TrimSpace(l.b)) == 0 {
			continue
		}
		req, perr := proto.ParseRequest(l.b)
		if perr != nil {
			logf("bad request: %v", perr)
			out.Write(enc.Error(req.ID, "invalid_request", perr.Error()))
			out.Flush()
			continue
		}
		switch req.Op {
		case "shutdown":
			code := closeDB()
			if code == 0 {
				out.Write(enc.OK(req.ID))
			} else {
				out.Write(enc.Error(req.ID, "internal_error", "close failed"))
			}
			out.Flush()
			os.Exit(code)
		case "execute":
		default:
			out.Write(enc.Error(req.ID, "unsupported_op", "unsupported op "+strconv.Quote(req.Op)))
			out.Flush()
			continue
		}
		if req.SQL == nil {
			out.Write(enc.Error(req.ID, "invalid_request", "missing sql"))
			out.Flush()
			continue
		}
		switch m := strings.ToLower(req.SearchMode); m {
		case "":
			db.SetSearchMode(defaultMode)
		case "exact", "ann":
			db.SetSearchMode(m)
		default:
			out.Write(enc.Error(req.ID, "invalid_request", "search_mode must be exact or ann"))
			out.Flush()
			continue
		}
		res, err := db.Exec(*req.SQL)
		switch {
		case err == nil && res.IsQuery, err == nil && res.HasAffected:
			out.Write(enc.Result(req.ID, res))
		case err == nil:
			out.Write(enc.OK(req.ID))
		case errors.Is(err, engine.ErrQuit):
			// Ending the session is the driver's job (op shutdown), not SQL's.
			out.Write(enc.SQLError(req.ID, types.ErrUnsupported("QUIT is not supported; use op shutdown")))
		default:
			var se *types.SQLError
			var fe *engine.FatalError
			if errors.As(err, &fe) {
				// Durability can no longer be guaranteed: report and stop.
				logf("%v", fe)
				out.Write(enc.Error(req.ID, "internal_error", fe.Error()))
				out.Flush()
				os.Exit(1)
			}
			if !errors.As(err, &se) {
				se = types.ErrInternal(err)
			}
			out.Write(enc.SQLError(req.ID, se))
		}
		out.Flush()
	}
}

func envInt(k string, def int64) int64 {
	if v, ok := os.LookupEnv(k); ok {
		if n, err := strconv.ParseInt(v, 10, 64); err == nil {
			return n
		}
	}
	return def
}

func envOr(k, def string) string {
	if v, ok := os.LookupEnv(k); ok {
		return v
	}
	return def
}
