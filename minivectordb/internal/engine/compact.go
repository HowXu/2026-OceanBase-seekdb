package engine

import (
	"encoding/binary"
	"encoding/json"
	"os"
	"path/filepath"
	"sort"

	"minivectordb/internal/storage"
)

// compactMinGarbage is the amount of dead log data that triggers a rewrite.
var compactMinGarbage int64 = 256 << 20

// maybeCompact rewrites the log with only live data when dead records
// (old row versions, deletes, aborted work) dominate. It runs at startup,
// before the database reports ready. The new log is written to a temp file,
// synced and renamed over the old one, so a crash at any point leaves either
// the old or the new complete log.
func (db *DB) maybeCompact() error {
	live := int64(0)
	for _, t := range db.byID {
		for _, off := range t.rows {
			live += int64(len(db.rowBytes(off))) + storage.HdrSize + 4
		}
	}
	garbage := db.log.Size() - live
	if garbage < compactMinGarbage || garbage < live {
		return nil
	}
	db.logf("compacting log: %d bytes, %d live", db.log.Size(), live)
	tmp := filepath.Join(db.dir, logName+".compact")
	_ = os.Remove(tmp)
	nl, err := storage.OpenLog(tmp, storage.SyncData)
	if err != nil {
		return err
	}
	ids := make([]uint32, 0, len(db.byID))
	for id := range db.byID {
		ids = append(ids, id)
	}
	sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
	newRows := make(map[uint32]map[int64]int64, len(ids))
	const txn = 1
	// All catalog records first: recovery discards pending row records when it
	// sees a record from another transaction, so rows must stay contiguous.
	for _, id := range ids {
		t := db.byID[id]
		b, _ := json.Marshal(&ddl{Op: "create_table", ID: t.ID, Name: t.Name, Cols: t.Cols, PK: t.PK})
		if _, err := nl.Append(storage.RecDDL, 0, b); err != nil {
			nl.Close()
			return err
		}
		if t.Index != nil && t.Index.Explicit {
			b, _ := json.Marshal(&ddl{Op: "create_index", ID: t.ID, Index: t.Index.Name})
			if _, err := nl.Append(storage.RecDDL, 0, b); err != nil {
				nl.Close()
				return err
			}
		}
	}
	for _, id := range ids {
		t := db.byID[id]
		m := make(map[int64]int64, len(t.rows))
		for pk, off := range t.rows {
			rec := nl.Begin(storage.RecPut, txn)
			buf := nl.Buf()
			*buf = binary.LittleEndian.AppendUint32(*buf, t.ID)
			*buf = append(*buf, db.rowBytes(off)...)
			p, err := nl.End(rec)
			if err != nil {
				nl.Close()
				return err
			}
			m[pk] = p + 4
		}
		newRows[id] = m
	}
	if _, err := nl.Append(storage.RecCommit, txn, nil); err != nil {
		nl.Close()
		return err
	}
	if err := nl.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmp, filepath.Join(db.dir, logName)); err != nil {
		return err
	}
	if err := syncDir(db.dir); err != nil {
		return err
	}
	old := db.log
	l, err := storage.OpenLog(filepath.Join(db.dir, logName), db.opts.Sync)
	if err != nil {
		return err
	}
	db.log = l
	old.Close()
	for id, m := range newRows {
		db.byID[id].rows = m
	}
	if db.nextTxn <= txn {
		db.nextTxn = txn + 1
	}
	return nil
}

func syncDir(dir string) error {
	f, err := os.Open(dir)
	if err != nil {
		return err
	}
	defer f.Close()
	return f.Sync()
}
