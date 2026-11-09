package engine

import (
	"encoding/binary"

	"minivectordb/internal/storage"
)

const deleted int64 = -1

// Txn holds uncommitted changes as an overlay over the committed row maps.
// Its log records are written as statements execute but only take effect at
// recovery if the COMMIT record made it to the log.
type Txn struct {
	id      uint64
	overlay map[uint32]map[int64]int64 // table -> pk -> row offset or deleted
	wrote   bool
}

func (db *DB) newTxn() *Txn {
	t := &Txn{id: db.nextTxn, overlay: map[uint32]map[int64]int64{}}
	db.nextTxn++
	return t
}

// change is one planned row modification produced by a statement before any
// of it is written, which keeps failed statements free of side effects.
type change struct {
	t   *Table
	pk  int64
	row []byte // encoded row; nil means delete
}

// lookup returns the visible row offset for pk.
func (db *DB) lookup(t *Table, pk int64) (int64, bool) {
	if db.txn != nil {
		if ov := db.txn.overlay[t.ID]; ov != nil {
			if off, ok := ov[pk]; ok {
				return off, off != deleted
			}
		}
	}
	off, ok := t.rows[pk]
	return off, ok
}

// forEach visits every visible row of t. Returning false stops the walk.
func (db *DB) forEach(t *Table, fn func(pk, off int64) bool) {
	var ov map[int64]int64
	if db.txn != nil {
		ov = db.txn.overlay[t.ID]
	}
	for pk, off := range t.rows {
		if ov != nil {
			if _, ok := ov[pk]; ok {
				continue
			}
		}
		if !fn(pk, off) {
			return
		}
	}
	for pk, off := range ov {
		if off != deleted && !fn(pk, off) {
			return
		}
	}
}

func (db *DB) visibleCount(t *Table) int {
	n := len(t.rows)
	if db.txn != nil {
		for pk, off := range db.txn.overlay[t.ID] {
			_, base := t.rows[pk]
			if base && off == deleted {
				n--
			} else if !base && off != deleted {
				n++
			}
		}
	}
	return n
}

// apply writes planned changes to the log under the given transaction and
// records them in its overlay.
func (db *DB) apply(tx *Txn, chs []change) error {
	for _, c := range chs {
		ov := tx.overlay[c.t.ID]
		if ov == nil {
			ov = map[int64]int64{}
			tx.overlay[c.t.ID] = ov
		}
		if c.row == nil {
			off := db.log.Begin(storage.RecDel, tx.id)
			b := db.log.Buf()
			*b = binary.LittleEndian.AppendUint32(*b, c.t.ID)
			*b = binary.LittleEndian.AppendUint64(*b, uint64(c.pk))
			if _, err := db.log.End(off); err != nil {
				return err
			}
			ov[c.pk] = deleted
		} else {
			off := db.log.Begin(storage.RecPut, tx.id)
			b := db.log.Buf()
			*b = binary.LittleEndian.AppendUint32(*b, c.t.ID)
			*b = append(*b, c.row...)
			p, err := db.log.End(off)
			if err != nil {
				return err
			}
			ov[c.pk] = p + 4
		}
		tx.wrote = true
	}
	return nil
}

// commitTxn makes tx durable and merges its overlay into the committed state.
func (db *DB) commitTxn(tx *Txn) error {
	if !tx.wrote {
		return nil
	}
	if _, err := db.log.Append(storage.RecCommit, tx.id, nil); err != nil {
		return err
	}
	if err := db.log.Sync(); err != nil {
		return err
	}
	for tid, ov := range tx.overlay {
		t := db.byID[tid]
		if t == nil {
			continue
		}
		for pk, off := range ov {
			if off == deleted {
				delete(t.rows, pk)
				if t.Index != nil {
					t.Index.remove(pk)
				}
			} else {
				t.rows[pk] = off
				if t.Index != nil {
					t.Index.upsert(db, t, pk, off)
				}
			}
		}
	}
	return nil
}

// abortTxn discards tx. The ABORT record is not synced: if it is lost, the
// missing COMMIT already makes recovery ignore the transaction.
func (db *DB) abortTxn(tx *Txn) {
	if tx.wrote {
		_, _ = db.log.Append(storage.RecAbort, tx.id, nil)
	}
}

func (db *DB) commit() error {
	tx := db.txn
	db.txn = nil
	return db.commitTxn(tx)
}

func (db *DB) rollback() {
	tx := db.txn
	db.txn = nil
	db.abortTxn(tx)
}

// write runs planned changes either inside the explicit transaction or as an
// autocommitted single-statement transaction.
func (db *DB) write(chs []change) error {
	if len(chs) == 0 {
		return nil
	}
	if db.txn != nil {
		return db.apply(db.txn, chs)
	}
	tx := db.newTxn()
	db.txn = tx // make lookups during apply see this statement
	err := db.apply(tx, chs)
	db.txn = nil
	if err != nil {
		db.abortTxn(tx)
		return err
	}
	return db.commitTxn(tx)
}
