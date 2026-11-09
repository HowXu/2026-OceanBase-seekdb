// Command minivectordb runs the database as a foreground process that reads
// SQL from stdin and writes one JSON result line per statement to stdout.
package main

import (
	"bufio"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/signal"
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
	mode := flag.String("mode", envOr("MVDB_SEARCH_MODE", "exact"), "vector search mode: exact or ann")
	sync := flag.String("sync", envOr("MVDB_SYNC", "fdatasync"), "commit durability: fdatasync or none")
	indexMB := flag.Int64("index-mem-mb", envInt("MVDB_INDEX_MEM_MB", 256), "memory budget for in-memory vectors per table")
	ready := flag.String("ready", envOr("MVDB_READY", "READY"), "line printed on stdout once recovery is done; empty to disable")
	flag.Parse()

	logf := func(format string, a ...any) { fmt.Fprintf(os.Stderr, "minivectordb: "+format+"\n", a...) }
	opts := engine.Options{SearchMode: strings.ToLower(*mode), Logf: logf, IndexMemBytes: *indexMB << 20}
	if lim := cgroupMemLimit(); lim > 0 {
		// memory.peak includes page cache, so keep the heap well below the
		// limit: vectors get at most a quarter, and the GC targets 60%.
		if q := lim / 4; q < opts.IndexMemBytes {
			opts.IndexMemBytes = q
		}
		debug.SetMemoryLimit(lim * 6 / 10)
		logf("cgroup memory limit %d MiB, vector budget %d MiB", lim>>20, opts.IndexMemBytes>>20)
	}
	switch *sync {
	case "none":
		opts.Sync = storage.SyncNone
	default:
		opts.Sync = storage.SyncData
	}
	if opts.SearchMode != "exact" && opts.SearchMode != "ann" {
		logf("invalid --mode %q", *mode)
		os.Exit(2)
	}
	db, err := engine.Open(*dataDir, opts)
	if err != nil {
		logf("open %s: %v", *dataDir, err)
		os.Exit(1)
	}

	out := bufio.NewWriterSize(os.Stdout, 1<<16)
	shutdown := func(code int) {
		out.Flush()
		if err := db.Close(); err != nil {
			logf("close: %v", err)
			code = 1
		}
		os.Exit(code)
	}
	// SIGTERM/SIGINT are a normal shutdown: roll back and flush. Statements
	// run on the main goroutine, so the signal is handled between statements.
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGTERM, syscall.SIGINT)

	type item struct {
		s  string
		ok bool
	}
	in := make(chan item)
	go func() {
		sp := proto.NewSplitter(os.Stdin)
		for {
			s, ok := sp.Next()
			in <- item{s, ok}
			if !ok {
				return
			}
		}
	}()

	if *ready != "" {
		fmt.Fprintln(out, *ready)
	}
	out.Flush()
	enc := &proto.Encoder{}
	for {
		var it item
		select {
		case <-sigs:
			shutdown(0)
		case it = <-in:
		}
		if !it.ok {
			shutdown(0)
		}
		if proto.IsDirective(it.s) {
			if handleDirective(db, it.s, out, enc) {
				shutdown(0)
			}
			continue
		}
		res, err := db.Exec(it.s)
		switch {
		case err == nil:
			out.Write(enc.Result(res))
		case errors.Is(err, engine.ErrQuit):
			shutdown(0)
		default:
			var se *types.SQLError
			var fe *engine.FatalError
			if errors.As(err, &fe) {
				logf("%v", fe)
				out.Flush()
				os.Exit(1)
			}
			if !errors.As(err, &se) {
				se = types.ErrInternal(err)
			}
			out.Write(enc.Error(se))
		}
		out.Flush()
	}
}

// handleDirective processes driver directives that reach the program. It
// returns true when the process should shut down.
func handleDirective(db *engine.DB, line string, out *bufio.Writer, enc *proto.Encoder) bool {
	f := strings.Fields(strings.TrimPrefix(line, "--"))
	if len(f) == 0 {
		return false
	}
	switch strings.ToLower(f[0]) {
	case "mode", "search_mode":
		if len(f) > 1 {
			m := strings.ToLower(f[1])
			if m == "exact" || m == "ann" {
				db.SetSearchMode(m)
			}
		}
	case "restart", "shutdown", "exit", "quit":
		return true
	}
	return false
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
