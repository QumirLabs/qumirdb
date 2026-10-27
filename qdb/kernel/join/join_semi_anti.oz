(block
  (pragma language overloads)

  ;; A SEMI / ANTI join that builds its left side marks the left rows the
  ;; right stream matches, as a streaming outer join marks its preserved build
  ;; rows (jt_probe_and_emit): the sign bit of the stored row id. Probe the left
  ;; table for `key` and mark every unmarked row the residual accepts; a marked
  ;; row needs no further residual call. Without a residual (key_decides set) a
  ;; match marks the whole bucket at once, so a marked first row means a marked
  ;; bucket. Generic over the dual key types like join_update.oz:
  ;; `stored_witness` only binds StoredKey.
  (fun jt_probe_and_mark [LookupKey StoredKey] ((var build <ref HashTable>)
                          (var key LookupKey)
                          (var stored_witness StoredKey)
                          (var right_row_id i64)
                          (var key_decides i64)
                          (var left_store <ptr TRowSet>)
                          (var right_store <ptr TRowSet>)
                          (var arena <ptr i8>)
                          (var stream_right_batch <ref TRowSet>)
                          (var hash u64)) -> bool
    (block
      (if (! (call jt_key_valid key)) (block (return #t)))
      (var build_keys =
        (cast (field build Keys) <ptr StoredKey>))
      (var build_slot = (call rh_lookup_dual build_keys (field build Ctrl)
                          (field build SlotId) (field build Capacity) key
                          hash))
      (if (== build_slot (: -1 i64)) (block (return #t)))
      (var build_aggs = (field build AggBuffers))
      (var build_counts = (index build_aggs (: 0 i64)))
      (var build_datas = (index build_aggs (: 2 i64)))
      (var bcount = (index build_counts build_slot))
      (var bdata = (cast (index build_datas build_slot) <ptr i64>))
      (if (&& (!= key_decides (: 0 i64)) (< (index bdata (: 0 i64)) (: 0 i64)))
        (block (return #t)))
      (var k i64)
      (= k (: 0 i64))
      (while (< k bcount)
        (block
          (var left_row_id = (index bdata k))
          (if (&& (>= left_row_id (: 0 i64))
                  (call jt_residual_filter left_store right_store arena
                    (index left_store (: 0 i64)) stream_right_batch
                    left_row_id right_row_id))
            (block
              (= bdata [k] (| left_row_id (<< (: 1 i64) (: 63 i64))))))
          (= k (+ k (: 1 i64)))))
      (return #t)))

  ;; Membership probing avoids duplicate SEMI rows from duplicate build keys.
  (fun jt_probe_semi [LookupKey StoredKey] ((var build <ref HashTable>)
                      (var key LookupKey)
                      (var stored_witness StoredKey)
                      (var row_id i64)
                      (var is_anti i64)
                      (var pairs <ref PairBuffer>)
                      (var hash u64)) -> bool
    (block
      (if (! (call jt_key_valid key))
        (block
          (if (!= is_anti 0)
            (block (return (call pb_push pairs row_id -1))))
          (return #t)))
      (var build_keys =
        (cast (field build Keys) <ptr StoredKey>))
      (var build_slot = (call rh_lookup_dual build_keys (field build Ctrl)
                          (field build SlotId) (field build Capacity) key
                          hash))
      (var matched = (!= build_slot -1))
      (var emit bool)
      (if (!= is_anti 0)
        (block (= emit (! matched)))
        (block (= emit matched)))
      (if emit
        (block
          (if (! (call pb_push pairs row_id -1))
            (block (return #f)))))
      (return #t))))
