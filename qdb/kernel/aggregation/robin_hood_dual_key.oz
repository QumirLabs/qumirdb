(block
  (pragma language overloads)

  ;; SwissTable probing (absl flat_hash_map) over the dual lookup/stored key
  ;; pair. Ctrl holds one byte per slot: 0x80 empty, otherwise H2 = hash & 0x7F.
  ;; Deletion is unsupported, so there are no tombstones and a probe stops at
  ;; the first group containing an empty slot. Target-selected groups are
  ;; 8 slots wide on ARM/portable and 16 on SSE2 (swiss_group.oz). Capacity is
  ;; a power of two and at least one group, and probing walks whole groups with
  ;; triangular steps, which visits every group exactly once.

  ;; Return the physical slot on a hit. On a miss, retain the first empty probe
  ;; slot so upsert can insert without walking the same groups again.
  (fun rh_lookup_or_empty_physical [StoredKey LookupKey]
       ((var keys <ptr StoredKey>)
        (var ctrl <ptr u8>)
        (var capacity i64)
        (var key LookupKey)
        (var hash u64)
        (var out_empty_slot <ref i64>)) -> i64
    (attrs inline)
    (block
      (= out_empty_slot (: -1 i64))
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
                (block (return slot)))
              (= m (call swiss_clear_first m))))
          (var empty = (call swiss_match_empty word))
          (if (!= empty (: 0 u64))
            (block
              (= out_empty_slot
                (+ (<< g group_shift) (call swiss_lowest_index empty)))
              (return (: -1 i64))))
          (= step (+ step (: 1 i64)))
          (= g (& (+ g step) group_mask))
          (= probes (+ probes (: 1 i64)))))
      (return (: -1 i64))))

  ;; Joins and dense-state aggregates retain physical-slot -> dense-ID mapping.
  (fun rh_lookup_or_empty_dual [StoredKey LookupKey]
       ((var keys <ptr StoredKey>) (var ctrl <ptr u8>)
        (var slot_ids <ptr i64>) (var capacity i64) (var key LookupKey)
        (var hash u64) (var out_empty_slot <ref i64>)) -> i64
    (block
      (var slot = (call rh_lookup_or_empty_physical
        keys ctrl capacity key hash out_empty_slot))
      (if (< slot 0) (block (return -1)))
      (return (index slot_ids slot))))

  (fun rh_lookup_dual [StoredKey LookupKey]
       ((var keys <ptr StoredKey>)
        (var ctrl <ptr u8>)
        (var slot_ids <ptr i64>)
        (var capacity i64)
        (var key LookupKey)
        (var hash u64)) -> i64
    (block
      (var empty_slot i64)
      (= empty_slot (: -1 i64))
      (return (call rh_lookup_or_empty_dual
        keys ctrl slot_ids capacity key hash empty_slot))))

  ;; Used for rehash and for insertion after a grow invalidates the saved slot.
  (fun rh_find_empty_slot
       ((var ctrl <ptr u8>)
        (var capacity i64)
        (var hash u64)) -> i64
    (block
      (var group_shift = (call swiss_group_shift))
      (var num_groups = (>> capacity group_shift))
      (var group_mask = (- num_groups (: 1 i64)))
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
              (return slot)))
          (= step (+ step (: 1 i64)))
          (= g (& (+ g step) group_mask))
          (= probes (+ probes (: 1 i64)))))
      (return (: -1 i64))))

  ;; Inserts a key known to be absent. Unlike Robin Hood this never moves a
  ;; resident entry: the first empty slot along the probe sequence wins.
  (fun rh_insert_stored [StoredKey]
       ((var keys <ptr StoredKey>)
        (var ctrl <ptr u8>)
        (var slot_ids <ptr i64>)
        (var capacity i64)
        (var key StoredKey)
        (var dense_slot i64)
        (var hash u64)) -> bool
    (block
      (var slot = (call rh_find_empty_slot ctrl capacity hash))
      (if (< slot (: 0 i64)) (block (return #f)))
      (= keys [slot] key)
      (= slot_ids [slot] dense_slot)
      (= ctrl [slot] (cast (& hash (: 127 u64)) u8))
      (return #t)))

  (fun aht_upsert_dual [LookupKey StoredKey]
       ((var ht <ref HashTable>) (var key LookupKey)
        (var stored_witness StoredKey) (var out_is_new <ref i64>)
        (var hash u64)) -> i64
    (attrs inline)
    (block
      (= out_is_new 0)
      (var empty_slot = -1)
      (var slot = (call rh_lookup_or_empty_physical
        (cast (field ht Keys) <ptr StoredKey>) (field ht Ctrl)
        (field ht Capacity) key hash empty_slot))
      (if (>= slot 0)
        (block
          (var slot_ids = (field ht SlotId))
          (return (index slot_ids slot))))
      (return (call aht_insert_dual ht key stored_witness out_is_new
        hash empty_slot #f))))

  ;; Returns -2 instead of growing a table that holds RowSwitchGroups groups
  ;; or more: the aggregate moves to the row layout (aggregation_rows.oz) and
  ;; continues there with this key.
  (fun aht_upsert_aggregate [LookupKey StoredKey]
       ((var ht <ref HashTable>) (var key LookupKey)
        (var stored_witness StoredKey) (var out_is_new <ref i64>)
        (var hash u64)) -> i64
    (attrs inline)
    (block
      (= out_is_new 0)
      (var empty_slot = -1)
      (var slot = (call rh_lookup_or_empty_physical
        (cast (field ht Keys) <ptr StoredKey>) (field ht Ctrl)
        (field ht Capacity) key hash empty_slot))
      (if (>= slot 0)
        (block (return slot)))
      (var switch_groups = (field ht RowSwitchGroups))
      (if (&& (> switch_groups 0) (>= (field ht Size) switch_groups))
        (block
          (if (call aht_insert_grows (field ht Size) (field ht Capacity))
            (block (return -2)))))
      (return (call aht_insert_dual ht key stored_witness out_is_new
        hash empty_slot #t))))

  ;; An insert grows the table before it would cross 7/8 of the capacity.
  (fun aht_insert_grows ((var size i64) (var capacity i64)) -> bool
    (attrs inline)
    (block (return (> (+ size 1) (- capacity (>> capacity 3))))))

  ;; Keep ownership, allocation, and growth off the common hit path.
  (fun aht_insert_dual [LookupKey StoredKey]
       ((var ht <ref HashTable>) (var key LookupKey)
        (var stored_witness StoredKey) (var out_is_new <ref i64>)
        (var hash u64) (var empty_slot i64) (var physical_states bool)) -> i64
    (block
      (var capacity = (field ht Capacity))
      (var keys = (cast (field ht Keys) <ptr StoredKey>))
      (var dense_slot = (field ht Size))
      (if (call aht_insert_grows dense_slot capacity)
        (block
          (if (> capacity (: 576460752303423487 i64))
            (block (return (: -1 i64))))
          (if (! (call aht_rehash_dual_impl
            ht (* capacity (: 2 i64)) stored_witness physical_states))
            (block (return (: -1 i64))))
          (= capacity (field ht Capacity))
          (= keys (cast (field ht Keys)
            <ptr StoredKey>))
          ;; Rehash invalidates the saved probe position. Only this path
          ;; needs a fresh insertion probe in the new table.
          (= empty_slot (call rh_find_empty_slot
            (field ht Ctrl) capacity hash))))
      ;; No table writes or owned-key allocations occur until a slot is known.
      (if (< empty_slot (: 0 i64)) (block (return (: -1 i64))))
      (var owned_bytes = (call key_owned_bytes key))
      (var owned_block = (cast (: 0 i64) <ptr u8>))
      (if (> owned_bytes (: 0 i64))
        (block
          (= owned_block (call aht_owned_arena_alloc ht owned_bytes))
          (if (== (cast owned_block i64) (: 0 i64))
            (block (return (: -1 i64))))))
      (var stored_key = (call key_clone_owned key owned_block))
      (var ctrl = (field ht Ctrl))
      (var slot_ids = (field ht SlotId))
      (= keys [empty_slot] stored_key)
      (if physical_states
        (block (= slot_ids [dense_slot] empty_slot))
        (block (= slot_ids [empty_slot] dense_slot)))
      (var state_slot = dense_slot)
      (if physical_states (block (= state_slot empty_slot)))
      (= ctrl [empty_slot] (cast (& hash (: 127 u64)) u8))
      (var group_keys = (cast (field ht GroupKeys)
        <ptr StoredKey>))
      (= group_keys [dense_slot] stored_key)
      (var agg_buffers = (field ht AggBuffers))
      (var agg i64)
      (= agg (: 0 i64))
      (while (< agg (field ht NumAggs))
        (block
          (var agg_buffer = (index agg_buffers agg))
          (= agg_buffer [state_slot] (: 0 i64))
          (= agg (+ agg (: 1 i64)))))
      (field_assign ht Size (+ dense_slot (: 1 i64)))
      (= out_is_new (: 1 i64))
      (return state_slot)))

  (fun rh_rehash_stored [StoredKey]
       ((var old_keys <ptr StoredKey>)
        (var old_ctrl <ptr u8>)
        (var old_slot_ids <ptr i64>)
        (var old_capacity i64)
        (var new_keys <ptr StoredKey>)
        (var new_ctrl <ptr u8>)
        (var new_slot_ids <ptr i64>)
        (var new_capacity i64)
        (var stored_witness StoredKey)) -> bool
    (block
      (var index i64)
      (= index (: 0 i64))
      (while (< index new_capacity)
        (block
          (= new_ctrl [index] (: 128 u8))
          (= new_slot_ids [index] (: -1 i64))
          (= index (+ index (: 1 i64)))))
      (= index (: 0 i64))
      (while (< index old_capacity)
        (block
          (if (< (index old_ctrl index) (: 128 u8))
            (block
              (if (! (call rh_insert_stored
                new_keys new_ctrl new_slot_ids new_capacity
                (index old_keys index) (index old_slot_ids index)
                (cast (call rh_hash (index old_keys index)) u64)))
                (block (return #f)))))
          (= index (+ index (: 1 i64)))))
      (return #t)))

  (fun aht_rehash_dual [StoredKey]
       ((var ht <ref HashTable>) (var new_capacity i64)
        (var stored_witness StoredKey)) -> bool
    (block (return (call aht_rehash_dual_impl
      ht new_capacity stored_witness #f))))

  ;; Scalar aggregate states use physical slots. SlotId then maps dense output
  ;; IDs to physical slots, so updates never load it; finalization loads it once
  ;; per group. GroupKeys remains dense and preserves output order.
  (fun rh_rehash_aggregate [StoredKey]
       ((var group_keys <ptr StoredKey>) (var size i64)
        (var new_keys <ptr StoredKey>) (var new_ctrl <ptr u8>)
        (var new_slot_ids <ptr i64>) (var new_capacity i64)) -> bool
    (block
      (var i = 0)
      (while (< i new_capacity)
        (block
          (= new_ctrl [i] (: 128 u8))
          (= new_slot_ids [i] -1)
          (= i (+ i 1))))
      (= i 0)
      (while (< i size)
        (block
          (var key = (index group_keys i))
          (var hash = (cast (call rh_hash key) u64))
          (var slot = (call rh_find_empty_slot new_ctrl new_capacity hash))
          (if (< slot 0) (block (return #f)))
          (= new_keys [slot] key)
          (= new_ctrl [slot] (cast (& hash (: 127 u64)) u8))
          (= new_slot_ids [i] slot)
          (= i (+ i 1))))
      (return #t)))

  (fun aht_rehash_dual_impl [StoredKey]
       ((var ht <ref HashTable>)
        (var new_capacity i64)
        (var stored_witness StoredKey)
        (var physical_states bool)) -> bool
    (block
      (var old_capacity = (field ht Capacity))
      (var size = (field ht Size))
      (var key_size = (field ht KeySize))
      (var num_aggs = (field ht NumAggs))
      ;; Rehash preserves the target-selected minimum group width.
      (if (|| (< new_capacity size) (< new_capacity (call swiss_group_width)))
        (block (return #f)))
      (if (!= (& new_capacity (- new_capacity (: 1 i64))) (: 0 i64))
        (block (return #f)))
      (if (|| (< key_size (: 1 i64))
              (> new_capacity (/ (: 9223372036854775807 i64) key_size)))
        (block (return #f)))
      (var key_bytes = (* new_capacity key_size))
      (var meta_bytes = (* new_capacity (: 8 i64)))
      (var new_keys = (cast (call qdb_alloc key_bytes) <ptr u8>))
      (var new_ctrl = (cast (call qdb_alloc new_capacity) <ptr u8>))
      (var new_slot_ids = (cast (call qdb_alloc meta_bytes) <ptr i64>))
      (var new_group_keys = (cast (call qdb_alloc key_bytes) <ptr u8>))
      (var new_agg_buffers = (cast (: 0 i64) <ptr <ptr i64>>))
      (if (> num_aggs (: 0 i64))
        (block
          (= new_agg_buffers (cast
            (call qdb_alloc (* num_aggs (: 8 i64))) <ptr <ptr i64>>))))
      (var allocation_failed bool)
      (= allocation_failed #f)
      (if (== (cast new_keys i64) (: 0 i64))
        (block (= allocation_failed #t)))
      (if (== (cast new_ctrl i64) (: 0 i64))
        (block (= allocation_failed #t)))
      (if (== (cast new_slot_ids i64) (: 0 i64))
        (block (= allocation_failed #t)))
      (if (== (cast new_group_keys i64) (: 0 i64))
        (block (= allocation_failed #t)))
      (if (&& (> num_aggs (: 0 i64))
              (== (cast new_agg_buffers i64) (: 0 i64)))
        (block (= allocation_failed #t)))
      (var allocated_aggs i64)
      (= allocated_aggs (: 0 i64))
      (if (! allocation_failed)
        (block
          (while (&& (! allocation_failed) (< allocated_aggs num_aggs))
            (block
              (= new_agg_buffers [allocated_aggs]
                (cast (call qdb_alloc meta_bytes) <ptr i64>))
              (if (== (cast (index new_agg_buffers allocated_aggs) i64)
                       (: 0 i64))
                (block (= allocation_failed #t)))
              (if (! allocation_failed)
                (block (= allocated_aggs (+ allocated_aggs (: 1 i64)))))))))
      (if allocation_failed
        (block
          (if (!= (cast new_keys i64) (: 0 i64))
            (block (call qdb_free (cast new_keys <ptr i8>))))
          (if (!= (cast new_ctrl i64) (: 0 i64))
            (block (call qdb_free (cast new_ctrl <ptr i8>))))
          (if (!= (cast new_slot_ids i64) (: 0 i64))
            (block (call qdb_free (cast new_slot_ids <ptr i8>))))
          (if (!= (cast new_group_keys i64) (: 0 i64))
            (block (call qdb_free (cast new_group_keys <ptr i8>))))
          (var cleanup_agg i64)
          (= cleanup_agg (: 0 i64))
          (while (< cleanup_agg allocated_aggs)
            (block
              (call qdb_free
                (cast (index new_agg_buffers cleanup_agg) <ptr i8>))
              (= cleanup_agg (+ cleanup_agg (: 1 i64)))))
          (if (!= (cast new_agg_buffers i64) (: 0 i64))
            (block (call qdb_free (cast new_agg_buffers <ptr i8>))))
          (return #f)))
      (var old_keys = (cast (field ht Keys)
        <ptr StoredKey>))
      (var new_keys_typed = (cast new_keys
        <ptr StoredKey>))
      (var rehashed bool)
      (if physical_states
        (block (= rehashed (call rh_rehash_aggregate
          (cast (field ht GroupKeys) <ptr StoredKey>) size
          new_keys_typed new_ctrl new_slot_ids new_capacity)))
        (block (= rehashed (call rh_rehash_stored
          old_keys (field ht Ctrl) (field ht SlotId) old_capacity
          new_keys_typed new_ctrl new_slot_ids new_capacity stored_witness))))
      (if (! rehashed)
        (block
          (call qdb_free (cast new_keys <ptr i8>))
          (call qdb_free (cast new_ctrl <ptr i8>))
          (call qdb_free (cast new_slot_ids <ptr i8>))
          (call qdb_free (cast new_group_keys <ptr i8>))
          (var cleanup_agg i64)
          (= cleanup_agg (: 0 i64))
          (while (< cleanup_agg num_aggs)
            (block
              (call qdb_free
                (cast (index new_agg_buffers cleanup_agg) <ptr i8>))
              (= cleanup_agg (+ cleanup_agg (: 1 i64)))))
          (if (!= (cast new_agg_buffers i64) (: 0 i64))
            (block (call qdb_free (cast new_agg_buffers <ptr i8>))))
          (return #f)))
      (var old_group_keys = (cast (field ht GroupKeys)
        <ptr StoredKey>))
      (var new_group_keys_typed = (cast new_group_keys
        <ptr StoredKey>))
      (var index i64)
      (= index (: 0 i64))
      (while (< index size)
        (block
          (= new_group_keys_typed [index] (index old_group_keys index))
          (= index (+ index (: 1 i64)))))
      (var old_slot_ids = (field ht SlotId))
      (var old_agg_buffers = (field ht AggBuffers))
      (var agg i64)
      (= agg (: 0 i64))
      (while (< agg num_aggs)
        (block
          (var old_buffer = (index old_agg_buffers agg))
          (var new_buffer = (index new_agg_buffers agg))
          (= index (: 0 i64))
          (while (< index size)
            (block
              (if physical_states
                (block
                  (= new_buffer [(index new_slot_ids index)]
                    (index old_buffer (index old_slot_ids index))))
                (block (= new_buffer [index] (index old_buffer index))))
              (= index (+ index (: 1 i64)))))
          (= agg (+ agg (: 1 i64)))))
      (call qdb_free (cast (field ht Keys) <ptr i8>))
      (call qdb_free (cast (field ht Ctrl) <ptr i8>))
      (call qdb_free (cast (field ht SlotId) <ptr i8>))
      (call qdb_free (cast (field ht GroupKeys) <ptr i8>))
      (= agg (: 0 i64))
      (while (< agg num_aggs)
        (block
          (call qdb_free (cast (index old_agg_buffers agg) <ptr i8>))
          (= agg (+ agg (: 1 i64)))))
      (if (!= (cast old_agg_buffers i64) (: 0 i64))
        (block (call qdb_free (cast old_agg_buffers <ptr i8>))))
      (field_assign ht Keys new_keys)
      (field_assign ht Ctrl new_ctrl)
      (field_assign ht SlotId new_slot_ids)
      (field_assign ht GroupKeys new_group_keys)
      (field_assign ht AggBuffers new_agg_buffers)
      (field_assign ht Capacity new_capacity)
      (return #t))))
