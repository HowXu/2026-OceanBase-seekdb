package engine

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"minivectordb/internal/storage"
	"minivectordb/internal/types"
)

const logName = "data.log"

type Options struct {
	Sync       storage.SyncMode
	SearchMode string // "exact" or "ann"
	// IndexMemBytes caps the in-memory vector copy per table.
	IndexMemBytes int64
	Logf          func(format string, a ...any)
}

type DB struct {
	dir     string
	log     *storage.Log
	tables  map[string]*Table // lower-cased name
	byID    map[uint32]*Table
	nextTID uint32
	nextTxn uint64
	txn     *Txn // explicit transaction, nil when autocommit
	opts    Options
	scratch []byte
}

// ddl is the JSON payload of a RecDDL record.
type ddl struct {
	Op     string         `json:"op"`
	ID     uint32         `json:"id"`
	Name   string         `json:"name,omitempty"`
	Cols   []types.Column `json:"cols,omitempty"`
	PK     int            `json:"pk,omitempty"`
	Column string         `json:"column,omitempty"`
	Index  string         `json:"index,omitempty"`
}

func (db *DB) logf(format string, a ...any) {
	if db.opts.Logf != nil {
		db.opts.Logf(format, a...)
	}
}

func Open(dir string, opts Options) (*DB, error) {
	if opts.SearchMode == "" {
		opts.SearchMode = "exact"
	}
	if opts.IndexMemBytes <= 0 {
		opts.IndexMemBytes = 256 << 20
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	l, err := storage.OpenLog(filepath.Join(dir, logName), opts.Sync)
	if err != nil {
		return nil, err
	}
	db := &DB{dir: dir, log: l, tables: map[string]*Table{}, byID: map[uint32]*Table{},
		nextTID: 1, nextTxn: 1, opts: opts}
	if err := db.recover(); err != nil {
		db.log.Close()
		return nil, err
	}
	if err := db.maybeCompact(); err != nil {
		db.log.Close()
		return nil, err
	}
	// Vector indexes are derived state, rebuilt before reporting ready.
	for _, t := range db.byID {
		if t.Index != nil {
			t.Index.rebuild(db, t)
		}
	}
	return db, nil
}

// recover replays the log. Records of a transaction are applied only when its
// COMMIT record is found; the log is then cut right after the last committed
// record, which also drops a torn tail. Replaying again is idempotent, so a
// crash during recovery is harmless.
func (db *DB) recover() error {
	sc := storage.NewScanner(db.log, 0)
	type pend struct {
		typ byte
		tid uint32
		pk  int64
		off int64
	}
	var pending []pend
	var curTxn uint64
	lastGood := int64(0)
	var r storage.Record
	for sc.Next(&r) {
		if r.Txn >= db.nextTxn {
			db.nextTxn = r.Txn + 1
		}
		if r.Txn != curTxn {
			pending = pending[:0]
			curTxn = r.Txn
		}
		switch r.Type {
		case storage.RecPut:
			tid := binary.LittleEndian.Uint32(r.Payload)
			pk := rowPK(r.Payload[4:])
			pending = append(pending, pend{storage.RecPut, tid, pk, r.Off + storage.HdrSize + 4})
		case storage.RecDel:
			tid := binary.LittleEndian.Uint32(r.Payload)
			pk := int64(binary.LittleEndian.Uint64(r.Payload[4:]))
			pending = append(pending, pend{storage.RecDel, tid, pk, 0})
		case storage.RecCommit:
			for _, p := range pending {
				t := db.byID[p.tid]
				if t == nil {
					continue
				}
				if p.typ == storage.RecPut {
					t.rows[p.pk] = p.off
				} else {
					delete(t.rows, p.pk)
				}
			}
			pending = pending[:0]
			lastGood = sc.Good
		case storage.RecAbort:
			pending = pending[:0]
			lastGood = sc.Good
		case storage.RecDDL:
			var d ddl
			if err := json.Unmarshal(r.Payload, &d); err != nil {
				return fmt.Errorf("corrupt DDL record at %d: %v", r.Off, err)
			}
			db.applyDDL(&d)
			pending = pending[:0]
			lastGood = sc.Good
		}
	}
	if sc.Err != nil {
		return sc.Err
	}
	if lastGood != db.log.Size() {
		db.logf("recovery: truncating log from %d to %d bytes", db.log.Size(), lastGood)
		if err := db.log.Truncate(lastGood); err != nil {
			return err
		}
	}
	return nil
}

func (db *DB) applyDDL(d *ddl) {
	switch d.Op {
	case "create_table":
		t := newTable(d.ID, d.Name, d.Cols, d.PK)
		db.tables[strings.ToLower(d.Name)] = t
		db.byID[d.ID] = t
		if d.ID >= db.nextTID {
			db.nextTID = d.ID + 1
		}
	case "drop_table":
		if t := db.byID[d.ID]; t != nil {
			delete(db.tables, strings.ToLower(t.Name))
			delete(db.byID, d.ID)
		}
	case "create_index":
		if t := db.byID[d.ID]; t != nil {
			t.Index = newVecIndex(d.Index)
			t.Index.Explicit = true
		}
	}
}

// writeDDL logs and applies a catalog change durably. Like MySQL, DDL first
// commits any open explicit transaction.
func (db *DB) writeDDL(d *ddl) error {
	if db.txn != nil {
		if err := db.commit(); err != nil {
			return err
		}
	}
	b, _ := json.Marshal(d)
	if _, err := db.log.Append(storage.RecDDL, 0, b); err != nil {
		return err
	}
	if err := db.log.Sync(); err != nil {
		return err
	}
	db.applyDDL(d)
	return nil
}

// rowBytes returns the encoded row stored at off.
func (db *DB) rowBytes(off int64) []byte {
	h := db.log.View(off-4-storage.HdrSize, 4)
	n := int(binary.LittleEndian.Uint32(h)) - 4
	return db.log.View(off, n)
}

func (db *DB) Close() error {
	if db.txn != nil {
		db.rollback()
	}
	return db.log.Close()
}

func (db *DB) SearchMode() string     { return db.opts.SearchMode }
func (db *DB) SetSearchMode(m string) { db.opts.SearchMode = m }
