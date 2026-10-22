(block
  (pragma language overloads)

  ;; Symmetric hash join probe+insert, generic over the dual key types.
  ;; The dense bucket lives in the reused aggregation HashTable's AggBuffers
  ;; (NumAggs=3: length / capacity / data-ptr-as-i64, see join_table.oz). Key
  ;; reading/assembly is generated per-query (join_gen.cpp); this generic core
  ;; takes the already-assembled <named LookupKey> plus a zero StoredKey
  ;; `stored_witness` that only binds the stored template type, and reuses the
  ;; aggregation dual-key helpers (rh_lookup_dual / aht_upsert_dual) so string
  ;; keys probe as StringView and are cloned into OwnedString storage on
  ;; insert. Fixed-width keys degenerate: LookupKey == StoredKey, cloning is a
  ;; plain copy.

  ;; Probe `build` for `key` and emit one pair per matching stored row. is_left
  ;; orders the emitted pair as (left, right): non-zero means the current row is
  ;; left, zero means it is right. A non-zero `mark` sets the sign bit of the
  ;; stored row id of every build row that gets a pair, so jt_emit_unmarked can
  ;; find the rows of a streaming outer join that never matched. Returns #f only
  ;; on PairBuffer allocation failure.
  (fun jt_probe_and_emit [LookupKey StoredKey] ((var build <ref HashTable>)
                          (var key LookupKey)
                          (var stored_witness StoredKey)
                          (var row_id i64)
                          (var is_left i64)
                          (var mark i64)
                          (var pairs <ref PairBuffer>)
                          (var left_store <ptr TRowSet>)
                          (var right_store <ptr TRowSet>)
                          (var stream_left_batch <ref TRowSet>)
                          (var stream_right_batch <ref TRowSet>)
                          (var hash u64)) -> bool
    (block
      (if (! (call jt_key_valid key)) (block (return #t)))
      (var build_keys =
        (cast (field build Keys) <ptr StoredKey>))
      (var build_slot = (call rh_lookup_dual build_keys (field build Ctrl)
                        (field build SlotId) (field build Capacity) key
                        hash))
      (if (!= build_slot (: -1 i64))
        (block
          (var build_aggs = (field build AggBuffers))
          (var build_counts = (index build_aggs (: 0 i64)))
          (var build_datas = (index build_aggs (: 2 i64)))
          (var bcount = (index build_counts build_slot))
          (var bdata = (cast (index build_datas build_slot) <ptr i64>))
          (var k i64)
          (= k (: 0 i64))
          (while (< k bcount)
            (block
              ;; Row ids are non-negative, so this only drops a mark.
              (var build_row_id =
                (& (index bdata k) (: 9223372036854775807 i64)))
              (var flt_l i64)
              (= flt_l row_id)
              (var flt_r i64)
              (= flt_r build_row_id)
              (if (== is_left (: 0 i64))
                (block
                  (= flt_l build_row_id)
                  (= flt_r row_id)))
              (if (call jt_residual_filter left_store right_store
                    stream_left_batch stream_right_batch flt_l flt_r)
                (block
                  (if (!= mark (: 0 i64))
                    (block
                      (= bdata [k]
                        (| build_row_id (<< (: 1 i64) (: 63 i64))))))
                  (if (!= is_left (: 0 i64))
                    (block
                      (if (! (call pb_push pairs row_id build_row_id))
                        (block (return #f))))
                    (block
                      (if (! (call pb_push pairs build_row_id row_id))
                        (block (return #f)))))))
              (= k (+ k (: 1 i64)))))))
      (return #t)))

  ;; Probe the opposite table for `key`, emit one pair per matching stored row,
  ;; then insert `own_row_id` under `key` into the own table. is_left orders the
  ;; emitted pair as (left, right). Returns #f only on allocation failure.
  (fun jt_emit_and_insert [LookupKey StoredKey] ((var own <ref HashTable>)
                           (var opp <ref HashTable>)
                           (var key LookupKey)
                           (var stored_witness StoredKey)
                           (var own_row_id i64)
                           (var is_left i64)
                           (var pairs <ref PairBuffer>)
                           (var left_store <ptr TRowSet>)
                           (var right_store <ptr TRowSet>)
                           (var hash u64)) -> bool
    (block
      (if (! (call jt_probe_and_emit opp key stored_witness own_row_id is_left
                    (: 0 i64) pairs left_store right_store
                    (index left_store (: 0 i64))
                    (index right_store (: 0 i64))
                    hash))
        (block (return #f)))
      (var is_new i64)
      (= is_new (: 0 i64))
      (var own_slot = (call aht_upsert_dual own key stored_witness is_new hash))
      (if (== own_slot (: -1 i64)) (block (return #f)))
      (if (! (call jb_append own own_slot own_row_id))
        (block (return #f)))
      (return #t)))

  ;; Streaming outer join finalizer: push every stored row of `build` that
  ;; jt_probe_and_emit left unmarked, with the null row id on the stream side.
  ;; is_left is non-zero when `build` holds the left rows.
  (fun jt_emit_unmarked ((var build <ref HashTable>)
                         (var is_left i64)
                         (var pairs <ref PairBuffer>)) -> bool
    (block
      (var aggs = (field build AggBuffers))
      (var counts = (index aggs (: 0 i64)))
      (var datas = (index aggs (: 2 i64)))
      (var size = (field build Size))
      (var slot i64)
      (= slot (: 0 i64))
      (while (< slot size)
        (block
          (var bcount = (index counts slot))
          (var bdata = (cast (index datas slot) <ptr i64>))
          (var k i64)
          (= k (: 0 i64))
          (while (< k bcount)
            (block
              (var build_row_id = (index bdata k))
              (if (>= build_row_id (: 0 i64))
                (block
                  (if (!= is_left (: 0 i64))
                    (block
                      (if (! (call pb_push pairs build_row_id (: -1 i64)))
                        (block (return #f))))
                    (block
                      (if (! (call pb_push pairs (: -1 i64) build_row_id))
                        (block (return #f)))))))
              (= k (+ k (: 1 i64)))))
          (= slot (+ slot (: 1 i64)))))
      (return #t)))

  ;; Insert a stored row under `key` without probing the opposite table.
  ;; Used by residual SEMI / ANTI: the right table is an i64 matched-id set, not
  ;; a join-key table, so left build must not call jt_probe_and_emit first.
  (fun jt_insert_row_only [LookupKey StoredKey] ((var own <ref HashTable>)
                           (var key LookupKey)
                           (var stored_witness StoredKey)
                           (var own_row_id i64)
                           (var hash u64)) -> bool
    (block
      (var is_new i64)
      (= is_new (: 0 i64))
      (var own_slot = (call aht_upsert_dual own key stored_witness is_new hash))
      (if (== own_slot (: -1 i64)) (block (return #f)))
      (if (! (call jb_append own own_slot own_row_id))
        (block (return #f)))
      (return #t)))

  ;; Insert only the key into own table without storing a row ID.
  ;; Used for the non-retained side (right side) of SEMI / ANTI joins —
  ;; the right table is queried for key existence only, not row materialization.
  (fun jt_insert_slot_only [LookupKey StoredKey] ((var own <ref HashTable>)
                            (var key LookupKey)
                            (var stored_witness StoredKey)
                            (var hash u64)) -> bool
    (block
      (var is_new i64)
      (= is_new (: 0 i64))
      (var own_slot = (call aht_upsert_dual own key stored_witness is_new hash))
      (if (== own_slot (: -1 i64)) (block (return #f)))
      (return #t))))
