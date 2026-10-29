(block
  (pragma language overloads)

  ;; Generic Robin Hood aggregation table over an external <ref HashTable>.
  ;; Key representation is visible only through the injected rh_hash(Key) /
  ;; rh_key_equal(Key,Key) overloads and the per-query agg_apply_reducers.
  ;; Key-owned storage (Keys/GroupKeys) is byte-addressed (<ptr u8>); typed
  ;; <ptr Key> pointers exist only locally via cast. KeySize lives in the
  ;; HashTable so rehash/grow never assumes a width.
  ;;
  ;; aht_init/aht_destroy are width-agnostic (pure byte allocation); only
  ;; aht_update/aht_rehash are generic over Key.

  ;; ---- pure generic probe leaves (no scalar pointers) ----

  ;; SwissTable probing; the Ctrl encoding and group walk match
  ;; robin_hood_dual_key.oz, see the comment there.
  (fun rh_lookup_slot [Key] ((var keys <ptr Key>)
                       (var ctrl <ptr u8>)
                       (var slot_ids <ptr i64>)
                       (var capacity i64)
                       (var key Key)) -> i64
    (block
      (var hash = (cast (call rh_hash key) u64))
      (var group_shift = (call swiss_group_shift))
      (var num_groups = (>> capacity group_shift))
      (var group_mask = (- num_groups (: 1 i64)))
      (var h2 = (& hash (: 127 u64)))
      (var g = (& (cast (>> hash (: 7 u64)) i64) group_mask))
      (var step i64)
      (= step (: 0 i64))
      (var probes i64)
      (= probes (: 0 i64))
      (while (< probes num_groups)
        (block
          (var word = (call swiss_group ctrl g))
          (var m = (call swiss_match word h2))
          (while (!= m (: 0 u64))
            (block
              (var slot = (+ (<< g group_shift) (call swiss_lowest_index m)))
              (if (call rh_key_equal (index keys slot) key)
                (block (return (index slot_ids slot))))
              (= m (call swiss_clear_first m))))
          (if (!= (call swiss_match_empty word) (: 0 u64))
            (block (return (: -1 i64))))
          (= step (+ step (: 1 i64)))
          (= g (& (+ g step) group_mask))
          (= probes (+ probes (: 1 i64)))))
      (return (: -1 i64))))

  ;; Inserts a key known to be absent, assigning it dense_slot. Nothing is
  ;; displaced: the first empty slot along the probe sequence wins. Returns #f
  ;; only when the table is unexpectedly full.
  (fun rh_insert_displace [Key] ((var keys <ptr Key>)
                           (var ctrl <ptr u8>)
                           (var slot_ids <ptr i64>)
                           (var capacity i64)
                           (var key Key)
                           (var dense_slot i64)) -> bool
    (block
      (var hash = (cast (call rh_hash key) u64))
      (var group_shift = (call swiss_group_shift))
      (var num_groups = (>> capacity group_shift))
      (var group_mask = (- num_groups (: 1 i64)))
      (var h2 = (& hash (: 127 u64)))
      (var g = (& (cast (>> hash (: 7 u64)) i64) group_mask))
      (var step i64)
      (= step (: 0 i64))
      (var probes i64)
      (= probes (: 0 i64))
      (while (< probes num_groups)
        (block
          (var empty = (call swiss_match_empty (call swiss_group ctrl g)))
          (if (!= empty (: 0 u64))
            (block
              (var slot = (+ (<< g group_shift) (call swiss_lowest_index empty)))
              (= keys [slot] key)
              (= slot_ids [slot] dense_slot)
              (= ctrl [slot] (cast h2 u8))
              (return #t)))
          (= step (+ step (: 1 i64)))
          (= g (& (+ g step) group_mask))
          (= probes (+ probes (: 1 i64)))))
      (return #f)))

  ;; ---- lifecycle over <ref HashTable> (byte storage) ----

  (fun aht_init ((var ht <ref HashTable>)
                 (var capacity i64)
                 (var num_aggs i64)
                 (var key_size i64)) -> bool
    (block
      ;; Preserve the host API minimum request; SSE2 uses a 16-slot group.
      (var min_capacity = (call swiss_group_width))
      (if (&& (== capacity 8) (< capacity min_capacity))
        (block (= capacity min_capacity)))
      (if (|| (< capacity min_capacity)
              (> capacity (: 1152921504606846975 i64)))
        (block (return #f)))
      (if (!= (& capacity (- capacity (: 1 i64))) (: 0 i64))
        (block (return #f)))
      (if (< num_aggs (: 0 i64)) (block (return #f)))
      (if (|| (< key_size (: 1 i64))
              (> capacity (/ (: 9223372036854775807 i64) key_size)))
        (block (return #f)))
      (var key_bytes = (* capacity key_size))
      (var meta_bytes = (* capacity (: 8 i64)))
      (var keys = (cast (call qdb_alloc key_bytes) <ptr u8>))
      (var ctrl = (cast (call qdb_alloc capacity) <ptr u8>))
      (var slot_ids = (cast (call qdb_alloc meta_bytes) <ptr i64>))
      (var group_keys = (cast (call qdb_alloc key_bytes) <ptr u8>))
      (var agg_buffers = (cast (call qdb_alloc (* num_aggs (: 8 i64))) <ptr <ptr i64>>))
      (var allocation_failed bool)
      (= allocation_failed #f)
      (if (== (cast keys i64) (: 0 i64)) (block (= allocation_failed #t)))
      (if (== (cast ctrl i64) (: 0 i64)) (block (= allocation_failed #t)))
      (if (== (cast slot_ids i64) (: 0 i64)) (block (= allocation_failed #t)))
      (if (== (cast group_keys i64) (: 0 i64)) (block (= allocation_failed #t)))
      (if (&& (> num_aggs (: 0 i64)) (== (cast agg_buffers i64) (: 0 i64)))
        (block (= allocation_failed #t)))
      (var a i64)
      (= a (: 0 i64))
      (if (! allocation_failed)
        (block
          (while (< a num_aggs)
            (block
              (= agg_buffers [a] (cast (call qdb_alloc meta_bytes) <ptr i64>))
              (if (== (cast (index agg_buffers a) i64) (: 0 i64))
                (block (= allocation_failed #t)))
              (= a (+ a (: 1 i64)))))))
      (if allocation_failed
        (block
          (if (!= (cast keys i64) (: 0 i64)) (block (call qdb_free (cast keys <ptr i8>))))
          (if (!= (cast ctrl i64) (: 0 i64)) (block (call qdb_free (cast ctrl <ptr i8>))))
          (if (!= (cast slot_ids i64) (: 0 i64)) (block (call qdb_free (cast slot_ids <ptr i8>))))
          (if (!= (cast group_keys i64) (: 0 i64)) (block (call qdb_free (cast group_keys <ptr i8>))))
          (if (!= (cast agg_buffers i64) (: 0 i64))
            (block
              (= a (: 0 i64))
              (while (< a num_aggs)
                (block
                  (if (!= (cast (index agg_buffers a) i64) (: 0 i64))
                    (block (call qdb_free (cast (index agg_buffers a) <ptr i8>))))
                  (= a (+ a (: 1 i64)))))
              (call qdb_free (cast agg_buffers <ptr i8>))))
          (return #f)))
      (var i i64)
      (= i (: 0 i64))
      (while (< i capacity)
        (block
          (= ctrl [i] (: 128 u8))
          (= slot_ids [i] (: -1 i64))
          (= i (+ i (: 1 i64)))))
      (= a (: 0 i64))
      (while (< a num_aggs)
        (block
          (var buf = (index agg_buffers a))
          (= i (: 0 i64))
          (while (< i capacity)
            (block
              (= buf [i] (: 0 i64))
              (= i (+ i (: 1 i64)))))
          (= a (+ a (: 1 i64)))))
      (field_assign ht Keys keys)
      (field_assign ht Ctrl ctrl)
      (field_assign ht SlotId slot_ids)
      (field_assign ht GroupKeys group_keys)
      (field_assign ht AggBuffers agg_buffers)
      (field_assign ht OwnedBlocks (cast (: 0 i64) <ptr <ptr u8>>))
      (field_assign ht OwnedBlockCount (: 0 i64))
      (field_assign ht OwnedBlockCapacity (: 0 i64))
      (field_assign ht Capacity capacity)
      (field_assign ht Size (: 0 i64))
      (field_assign ht NumAggs num_aggs)
      (field_assign ht NumKeys (: 1 i64))
      (field_assign ht KeySize key_size)
      (return #t)))

  ;; Frees the slot arrays of the swiss table and keeps the owned key arena:
  ;; the stored keys of a table that moves to the row layout
  ;; (aggregation/aggregation_rows.oz) point into it.
  (fun aht_destroy_slots ((var ht <ref HashTable>))
    (block
      (var agg_buffers = (field ht AggBuffers))
      (var num_aggs = (field ht NumAggs))
      (if (!= (cast agg_buffers i64) (: 0 i64))
        (block
          (var a i64)
          (= a (: 0 i64))
          (while (< a num_aggs)
            (block
              (var buf = (index agg_buffers a))
              (if (!= (cast buf i64) (: 0 i64))
                (block (call qdb_free (cast buf <ptr i8>))))
              (= a (+ a (: 1 i64)))))
          (call qdb_free (cast agg_buffers <ptr i8>))))
      (if (!= (cast (field ht Keys) i64) (: 0 i64)) (block (call qdb_free (cast (field ht Keys) <ptr i8>))))
      (if (!= (cast (field ht Ctrl) i64) (: 0 i64)) (block (call qdb_free (cast (field ht Ctrl) <ptr i8>))))
      (if (!= (cast (field ht SlotId) i64) (: 0 i64)) (block (call qdb_free (cast (field ht SlotId) <ptr i8>))))
      (if (!= (cast (field ht GroupKeys) i64) (: 0 i64)) (block (call qdb_free (cast (field ht GroupKeys) <ptr i8>))))
      (field_assign ht Keys (cast (: 0 i64) <ptr u8>))
      (field_assign ht Ctrl (cast (: 0 i64) <ptr u8>))
      (field_assign ht SlotId (cast (: 0 i64) <ptr i64>))
      (field_assign ht GroupKeys (cast (: 0 i64) <ptr u8>))
      (field_assign ht AggBuffers (cast (: 0 i64) <ptr <ptr i64>>))
      (field_assign ht Capacity (: 0 i64))))

  (fun aht_destroy ((var ht <ref HashTable>))
    (block
      (call aht_destroy_slots ht)
      (call owned_arena_destroy
        (field ht OwnedBlocks)
        (field ht OwnedBlockCount)
        (field ht OwnedBlockCapacity))
      (field_assign ht Size (: 0 i64))
      (field_assign ht NumAggs (: 0 i64))))

  (fun aht_rehash [Key] ((var ht <ref HashTable>)
                   (var new_capacity i64)
                   (var key_witness Key)) -> bool
    (block
      (var key_size = (field ht KeySize))
      (if (|| (< new_capacity (: 1 i64))
              (> new_capacity (: 1152921504606846975 i64)))
        (block (return #f)))
      (if (> new_capacity (/ (: 9223372036854775807 i64) key_size))
        (block (return #f)))
      (var num_aggs = (field ht NumAggs))
      (var size = (field ht Size))
      (if (< new_capacity size) (block (return #f)))
      (var key_bytes = (* new_capacity key_size))
      (var meta_bytes = (* new_capacity (: 8 i64)))
      (var new_keys = (cast (call qdb_alloc key_bytes) <ptr u8>))
      (var new_ctrl = (cast (call qdb_alloc new_capacity) <ptr u8>))
      (var new_slot_ids = (cast (call qdb_alloc meta_bytes) <ptr i64>))
      (var new_group_keys = (cast (call qdb_alloc key_bytes) <ptr u8>))
      (var new_agg_buffers = (cast (call qdb_alloc (* num_aggs (: 8 i64))) <ptr <ptr i64>>))
      (var allocation_failed bool)
      (= allocation_failed #f)
      (if (== (cast new_keys i64) (: 0 i64)) (block (= allocation_failed #t)))
      (if (== (cast new_ctrl i64) (: 0 i64)) (block (= allocation_failed #t)))
      (if (== (cast new_slot_ids i64) (: 0 i64)) (block (= allocation_failed #t)))
      (if (== (cast new_group_keys i64) (: 0 i64)) (block (= allocation_failed #t)))
      (if (&& (> num_aggs (: 0 i64)) (== (cast new_agg_buffers i64) (: 0 i64)))
        (block (= allocation_failed #t)))
      (var a i64)
      (= a (: 0 i64))
      (if (! allocation_failed)
        (block
          (while (< a num_aggs)
            (block
              (= new_agg_buffers [a] (cast (call qdb_alloc meta_bytes) <ptr i64>))
              (if (== (cast (index new_agg_buffers a) i64) (: 0 i64))
                (block (= allocation_failed #t)))
              (= a (+ a (: 1 i64)))))))
      (if allocation_failed
        (block
          (if (!= (cast new_keys i64) (: 0 i64)) (block (call qdb_free (cast new_keys <ptr i8>))))
          (if (!= (cast new_ctrl i64) (: 0 i64)) (block (call qdb_free (cast new_ctrl <ptr i8>))))
          (if (!= (cast new_slot_ids i64) (: 0 i64)) (block (call qdb_free (cast new_slot_ids <ptr i8>))))
          (if (!= (cast new_group_keys i64) (: 0 i64)) (block (call qdb_free (cast new_group_keys <ptr i8>))))
          (if (!= (cast new_agg_buffers i64) (: 0 i64))
            (block
              (= a (: 0 i64))
              (while (< a num_aggs)
                (block
                  (if (!= (cast (index new_agg_buffers a) i64) (: 0 i64))
                    (block (call qdb_free (cast (index new_agg_buffers a) <ptr i8>))))
                  (= a (+ a (: 1 i64)))))
              (call qdb_free (cast new_agg_buffers <ptr i8>))))
          (return #f)))
      (var old_keys_typed =
        (cast (field ht Keys) <ptr Key>))
      (var new_keys_typed =
        (cast new_keys <ptr Key>))
      (if (! (call rh_rehash_into old_keys_typed (field ht Ctrl) (field ht SlotId)
                    (field ht Capacity) new_keys_typed new_ctrl new_slot_ids
                    new_capacity))
        (block
          (call qdb_free (cast new_keys <ptr i8>))
          (call qdb_free (cast new_ctrl <ptr i8>))
          (call qdb_free (cast new_slot_ids <ptr i8>))
          (call qdb_free (cast new_group_keys <ptr i8>))
          (= a (: 0 i64))
          (while (< a num_aggs)
            (block
              (call qdb_free (cast (index new_agg_buffers a) <ptr i8>))
              (= a (+ a (: 1 i64)))))
          (call qdb_free (cast new_agg_buffers <ptr i8>))
          (return #f)))
      (var old_group_typed =
        (cast (field ht GroupKeys) <ptr Key>))
      (var new_group_typed =
        (cast new_group_keys <ptr Key>))
      (var old_agg_buffers = (field ht AggBuffers))
      (var i i64)
      (= i (: 0 i64))
      (while (< i size)
        (block
          (= new_group_typed [i] (index old_group_typed i))
          (= i (+ i (: 1 i64)))))
      (= a (: 0 i64))
      (while (< a num_aggs)
        (block
          (var old_buf = (index old_agg_buffers a))
          (var new_buf = (index new_agg_buffers a))
          (= i (: 0 i64))
          (while (< i size)
            (block
              (= new_buf [i] (index old_buf i))
              (= i (+ i (: 1 i64)))))
          (= a (+ a (: 1 i64)))))
      (call qdb_free (cast (field ht Keys) <ptr i8>))
      (call qdb_free (cast (field ht Ctrl) <ptr i8>))
      (call qdb_free (cast (field ht SlotId) <ptr i8>))
      (call qdb_free (cast (field ht GroupKeys) <ptr i8>))
      (= a (: 0 i64))
      (while (< a num_aggs)
        (block
          (call qdb_free (cast (index old_agg_buffers a) <ptr i8>))
          (= a (+ a (: 1 i64)))))
      (call qdb_free (cast old_agg_buffers <ptr i8>))
      (field_assign ht Keys new_keys)
      (field_assign ht Ctrl new_ctrl)
      (field_assign ht SlotId new_slot_ids)
      (field_assign ht GroupKeys new_group_keys)
      (field_assign ht AggBuffers new_agg_buffers)
      (field_assign ht Capacity new_capacity)
      (return #t)))

  (fun aht_update [Key] ((var ht <ref HashTable>)
                   (var key Key)
                   (var value i64)) -> i64
    (block
      (var keys_typed =
        (cast (field ht Keys) <ptr Key>))
      (var dense_slot = (call rh_lookup_slot keys_typed (field ht Ctrl)
                          (field ht SlotId) (field ht Capacity) key))
      (if (!= dense_slot (: -1 i64))
        (block
          (call agg_apply_reducers (field ht AggBuffers) dense_slot value #f)
          (return dense_slot)))
      (var capacity = (field ht Capacity))
      (var size = (field ht Size))
      (if (> (+ size (: 1 i64)) (- capacity (>> capacity (: 3 i64))))
        (block
          (if (> capacity (: 576460752303423487 i64)) (block (return (: -1 i64))))
          (if (! (call aht_rehash ht (* capacity (: 2 i64)) key))
            (block (return (: -1 i64))))
          (= capacity (field ht Capacity))))
      (= keys_typed
        (cast (field ht Keys) <ptr Key>))
      (if (! (call rh_insert_displace keys_typed (field ht Ctrl) (field ht SlotId)
                    capacity key size))
        (block (return (: -1 i64))))
      (var group_typed =
        (cast (field ht GroupKeys) <ptr Key>))
      (= group_typed [size] key)
      (var agg_buffers = (field ht AggBuffers))
      (var num_aggs = (field ht NumAggs))
      (var a i64)
      (= a (: 0 i64))
      (while (< a num_aggs)
        (block
          (var buf = (index agg_buffers a))
          (= buf [size] (: 0 i64))
          (= a (+ a (: 1 i64)))))
      (call agg_apply_reducers agg_buffers size value #t)
      (field_assign ht Size (+ size (: 1 i64)))
      (return size))))
