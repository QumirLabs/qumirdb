(block
  (pragma language overloads)

  ;; Residual SEMI/ANTI keeps the left join table as the build side and uses the
  ;; right HashTable as an i64 set of matched left RowIds.
  (fun jt_init_matched ((var ht <ref HashTable>) (var capacity i64)) -> bool
    (block
      (return (call aht_init ht capacity (: 0 i64) (: 8 i64)))))

  (fun jt_destroy_matched ((var ht <ref HashTable>))
    (block
      (call aht_destroy ht)))

  (fun jt_matched_contains ((var matched <ref HashTable>)
                            (var left_id i64)) -> bool
    (block
      (var keys = (cast (field matched Keys) <ptr i64>))
      (var slot = (call rh_lookup_slot keys (field matched Ctrl)
                    (field matched SlotId) (field matched Capacity) left_id))
      (return (!= slot (: -1 i64)))))

  (fun jt_mark_matched ((var matched <ref HashTable>)
                        (var left_id i64)) -> bool
    (block
      (var keys = (cast (field matched Keys) <ptr i64>))
      (var slot = (call rh_lookup_slot keys (field matched Ctrl)
                    (field matched SlotId) (field matched Capacity) left_id))
      (if (!= slot (: -1 i64)) (block (return #t)))
      (var capacity = (field matched Capacity))
      (var size = (field matched Size))
      (if (> (+ size (: 1 i64)) (- capacity (/ capacity (: 4 i64))))
        (block
          (if (> capacity (: 576460752303423487 i64)) (block (return #f)))
          (if (! (call aht_rehash matched (* capacity (: 2 i64)) left_id))
            (block (return #f)))
          (= capacity (field matched Capacity))))
      (= keys (cast (field matched Keys) <ptr i64>))
      (if (! (call rh_insert_displace keys (field matched Ctrl)
                (field matched SlotId) capacity left_id size))
        (block (return #f)))
      (var group_keys = (cast (field matched GroupKeys) <ptr i64>))
      (= group_keys [size] left_id)
      (field_assign matched Size (+ size (: 1 i64)))
      (return #t)))

  ;; Probe the left build table for `key` and mark every matching left row id
  ;; (that passes the residual filter) in the matched set. Generic over the dual
  ;; key types like join_update.oz: `stored_witness` only binds StoredKey.
  (fun jt_probe_and_mark [LookupKey StoredKey] ((var build <ref HashTable>)
                          (var matched <ref HashTable>)
                          (var key LookupKey)
                          (var stored_witness StoredKey)
                          (var right_row_id i64)
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
              (var left_row_id = (index bdata k))
              (if (call jt_residual_filter left_store right_store arena
                    (index left_store (: 0 i64)) stream_right_batch
                    left_row_id right_row_id)
                (block
                  (if (! (call jt_mark_matched matched left_row_id))
                    (block (return #f)))))
              (= k (+ k (: 1 i64)))))))
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
      (return #t)))

  (fun jt_finalize_residual_semi_anti ((var matched <ref HashTable>)
                                       (var left_store <ptr TRowSet>)
                                       (var batch_count i64)
                                       (var is_anti i64)
                                       (var pairs <ref PairBuffer>)) -> bool
    (block
      (var b i64)
      (= b (: 0 i64))
      (while (< b batch_count)
        (block
          (var batch = (index left_store b))
          (var selection = (field batch Selection))
          (var selection_is_null = (== (cast selection i64) (: 0 i64)))
          (var r i64)
          (= r (: 0 i64))
          (while (< r (field batch RowCount))
            (block
              (if (|| selection_is_null (!= (index selection r) (: 0 u8)))
                (block
                  (var left_id = (+ (<< b (: 32 i64)) (& r (: 4294967295 i64))))
                  (var matched_row = (call jt_matched_contains matched left_id))
                  (var emit bool)
                  (if (!= is_anti (: 0 i64))
                    (block (= emit (! matched_row)))
                    (block (= emit matched_row)))
                  (if emit
                    (block
                      (if (! (call pb_push pairs left_id (: -1 i64)))
                        (block (return #f)))))))
              (= r (+ r (: 1 i64)))))
          (= b (+ b (: 1 i64)))))
      (return #t))))
