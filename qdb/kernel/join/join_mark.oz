(block
  (pragma language overloads)

  ;; Mark join (Neumann, Leis, Kemper, "The Complete Story of Joins (in HyPer)",
  ;; BTW 2017, section 3.3): every left row is emitted exactly once, followed by
  ;; a marker holding the SQL value of `key IN S`, where S is the set of right
  ;; keys. The right side is a key-only set (jt_insert_key_only), so duplicate
  ;; right keys never duplicate left rows. A pair holds the left row id and, in
  ;; place of a right row id, the marker code: 1 TRUE, 0 FALSE, -1 NULL.

  ;; Ordinary key equality groups every NULL key into one slot, so S contains
  ;; NULL iff the NULL key finds a slot. `null_key` is the zero key, whose
  ;; validity flags are clear; a key type without validity flags is never NULL.
  (fun mj_set_has_null [StoredKey] ((var right <ref HashTable>)
                                    (var null_key StoredKey)) -> bool
    (block
      (if (call jt_key_valid null_key) (block (return #f)))
      (var slot = (call rh_lookup_dual (cast (field right Keys) <ptr StoredKey>)
                    (field right Ctrl) (field right SlotId) (field right Capacity)
                    null_key (cast (call rh_hash null_key) u64)))
      (return (!= slot -1))))

  ;; `key IN S` is TRUE if S has a key equal to `key`; otherwise NULL if S is
  ;; non-empty and `key` is NULL or S contains NULL; otherwise FALSE.
  (fun mj_marker [LookupKey StoredKey] ((var right <ref HashTable>)
                                        (var key LookupKey)
                                        (var stored_witness StoredKey)
                                        (var right_has_null bool)
                                        (var hash u64)) -> i64
    (block
      (if (== (field right Size) 0) (block (return 0)))
      (if (! (call jt_key_valid key)) (block (return -1)))
      (var slot = (call rh_lookup_dual (cast (field right Keys) <ptr StoredKey>)
                    (field right Ctrl) (field right SlotId) (field right Capacity)
                    key hash))
      (if (!= slot -1) (block (return 1)))
      (if right_has_null (block (return -1)))
      (return 0)))

  ;; Emits every stored left row with the marker of its group key. Runs once
  ;; both sides are complete; `null_key` is the zero StoredKey.
  (fun mj_finalize [StoredKey] ((var left <ref HashTable>)
                                (var right <ref HashTable>)
                                (var null_key StoredKey)
                                (var pairs <ref PairBuffer>)) -> bool
    (block
      (var right_has_null = (call mj_set_has_null right null_key))
      (var group_keys = (cast (field left GroupKeys) <ptr StoredKey>))
      (var aggs = (field left AggBuffers))
      (var counts = (index aggs 0))
      (var datas = (index aggs 2))
      (var slot i64)
      (= slot 0)
      (while (< slot (field left Size))
        (block
          (var key = (index group_keys slot))
          (var marker = (call mj_marker right key null_key right_has_null
                          (cast (call rh_hash key) u64)))
          (var count = (index counts slot))
          (var rows = (cast (index datas slot) <ptr i64>))
          (var k i64)
          (= k 0)
          (while (< k count)
            (block
              (if (! (call pb_push pairs (index rows k) marker))
                (block (return #f)))
              (= k (+ k 1))))
          (= slot (+ slot 1))))
      (return #t)))

  ;; Output materializer worker for the marker column (see jt_materialize).
  (fun mj_materialize_marker_column
       ((var pairs <ref PairBuffer>)
        (var start i64)
        (var n i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var mask_owner_idx i64))
    (block
      (var byte_count = (call jm_mask_bytes n))
      (var data = (cast (call qdb_alloc byte_count) <ptr u8>))
      (var mask = (cast (call qdb_alloc byte_count) <ptr u8>))
      (call jm_record_owner out data_owner_idx (cast data <ptr i8>))
      (call jm_record_owner out mask_owner_idx (cast mask <ptr i8>))
      (field_assign out_col Data (cast data <ptr i8>))
      (field_assign out_col DataBitOffset (: 0 i32))
      (field_assign out_col Mask mask)
      (field_assign out_col MaskBitOffset (: 0 i32))
      (field_assign out_col Offsets (cast 0 <ptr i64>))
      (field_assign out_col OffsetWidth (: 0 u8))

      (var pair_data = (field pairs Data))
      (var i i64)
      (= i 0)
      (while (< i n)
        (block
          (var marker =
            (index pair_data (+ (* (+ start i) 2) 1)))
          (call qdb_bitmap_set_valid data i (== marker 1))
          (call qdb_bitmap_set_valid mask i (!= marker -1))
          (= i (+ i 1)))))))
