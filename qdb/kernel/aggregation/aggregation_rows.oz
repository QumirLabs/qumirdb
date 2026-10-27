(block
  (pragma language overloads)

  ;; Row layout of the aggregate table, on the row table (kernel/row_table.oz).
  ;; A group is one arena row:
  ;;   [0, 8)                 hash of the key;
  ;;   [key_offset, + key)    the Key struct;
  ;;   [state_offset, ...)    NumAggs i64 state words, in the order of the
  ;;                          swiss table's AggBuffers.
  ;; An update reads one entry and one row, where the swiss table reads its
  ;; control bytes, its key and one state array per state word, so the row
  ;; layout wins once the table outgrows the cache, the sooner the more state
  ;; words a group has. Keys and states are fixed-width; the offsets come
  ;; from the generated code (gen.cpp).
  ;;
  ;; Find-or-insert works on a chunk of rows in the passes of the row table:
  ;; the generated code stores keys and hashes, rt_seek_all walks to the
  ;; candidates, agr_find_or_insert compares or inserts, and the generated code
  ;; applies each row to its group's states. Groups never move: growth rewrites
  ;; the entries from the hashes kept in the rows, and finalization scans the
  ;; arena, which lists the groups in insertion order.
  ;;
  ;; The table is in this layout once RowTable has entries. An aggregate
  ;; starts here (agr_init) or moves here from the swiss table when that would
  ;; grow past RowSwitchGroups (agr_from_swiss).

  (fun agr_active ((var ht <ref HashTable>)) -> bool
    (attrs inline)
    (block (return (!= (cast (field (field ht RowTable) Entries) i64) 0))))

  ;; An empty table in the row layout with entries for `groups` groups.
  (fun agr_init ((var ht <ref HashTable>) (var groups i64) (var num_aggs i64)
                 (var key_size i64) (var row_bytes i64) (var block_rows i64)) -> bool
    (block
      (call ra_init (field ht RowArena) row_bytes block_rows)
      (if (! (call rt_init (field ht RowTable) groups)) (block (return #f)))
      (field_assign ht Size 0)
      (field_assign ht NumAggs num_aggs)
      (field_assign ht NumKeys 1)
      (field_assign ht KeySize key_size)
      (return #t)))

  ;; Frees the table in either layout.
  (fun agr_destroy ((var ht <ref HashTable>))
    (block
      (call aht_destroy ht)
      (call ra_destroy (field ht RowArena))
      (call rt_destroy (field ht RowTable))
      (call rt_scratch_destroy (field ht RowScratch))))

  ;; Stores the entry of a row whose key no other row has: the first empty
  ;; entry from its home.
  (fun agr_link ((var table <ref RowTable>) (var row <ptr u8>) (var hash u64))
    (attrs inline)
    (block
      (var entries = (field table Entries))
      (var mask = (field table Mask))
      (var h = (call rt_mix hash))
      (var off = (>> h (cast (field table Shift) u64)))
      (while (!= (index entries (cast off i64)) (: 0 u64))
        (block (= off (& (+ off (: 1 u64)) mask))))
      (= entries [(cast off i64)] (call rt_entry (call rt_salt h) row))))

  ;; New entries for `groups` groups, filled by a scan of the arena. The old
  ;; entries stay until the new ones are allocated.
  (fun agr_reindex ((var table <ref RowTable>) (var arena <ref RowArena>)
                    (var groups i64)) -> bool
    (block
      (var old = (field table Entries))
      (if (! (call rt_init table groups)) (block (return #f)))
      (call qdb_free (cast old <ptr i8>))
      (var blocks = (field arena Blocks))
      (var count = (field arena BlockCount))
      (var row_bytes = (field arena RowBytes))
      (var b = 0)
      (while (< b count)
        (block
          (var rows_in_block = (field arena BlockRows))
          (if (== b (- count 1)) (block (= rows_in_block (field arena CurRows))))
          (var row = (cast (index blocks b) i64))
          (var end = (+ row (* rows_in_block row_bytes)))
          (while (< row end)
            (block
              (var bytes = (cast row <ptr u8>))
              (call agr_link table bytes (call rt_row_hash bytes))
              (= row (+ row row_bytes))))
          (= b (+ b 1))))
      (return #t)))

  ;; Points the rows in Work[0, count) at their home entries again.
  (fun agr_rehome_work ((var table <ref RowTable>) (var s <ref RowScratch>) (var count i64))
    (block
      (var hashes = (field s Hashes))
      (var offsets = (field s Offsets))
      (var work = (field s Work))
      (var shift = (cast (field table Shift) u64))
      (var q = 0)
      (while (< q count)
        (block
          (var j = (index work q))
          (= offsets [j] (>> (call rt_mix (index hashes j)) shift))
          (= q (+ q 1))))))

  ;; Finds or inserts the group of every row j in [0, count) of the scratch,
  ;; whose key (at Keys + j * key_stride) and Hashes[j] the caller stored. The
  ;; stride keeps the key at the alignment its type needs, as key_offset does
  ;; in the row. Afterwards Entries[j] is the
  ;; group's row; bit 0 is set for the row that inserted the group, so that the
  ;; caller applies it to zero states as a new group (rows are 8-byte aligned).
  ;;
  ;; A pass reads each candidate entry again rather than trusting the seek: an
  ;; earlier row of the chunk may have inserted there since, with the same key
  ;; or another one. Rows with equal keys walk the same entries, so the first of
  ;; them inserts the group and the others find it. A row whose candidate holds
  ;; another key moves past it and seeks again with the next pass.
  (fun agr_find_or_insert [Key] ((var ht <ref HashTable>) (var count i64)
                                 (var key_offset i64) (var key_stride i64)
                                 (var state_offset i64) (var witness <ptr Key>)) -> bool
    (block
      (var hashes = (field (field ht RowScratch) Hashes))
      (var salts = (field (field ht RowScratch) Salts))
      (var offsets = (field (field ht RowScratch) Offsets))
      (var found = (field (field ht RowScratch) Entries))
      (var work = (field (field ht RowScratch) Work))
      (var keys = (cast (field (field ht RowScratch) Keys) i64))
      (var num_aggs = (field ht NumAggs))
      (call rt_seek_all (field ht RowTable) (field ht RowScratch) count)
      (var k = count)
      (var first = #t)
      (while (> k 0)
        (block
          ;; Only a row at an empty entry can insert, and at most one group, so
          ;; the entries grow here when those rows could fill more than half of
          ;; them. The pending rows then seek again from their homes, where any
          ;; of them may meet an empty entry, so the new entries have room for
          ;; all of them. The rows do not move.
          (var empty = 0)
          (var q = 0)
          (while (< q k)
            (block
              (var j = q)
              (if (! first) (block (= j (index work q))))
              (if (== (index found j) (: 0 u64)) (block (= empty (+ empty 1))))
              (= q (+ q 1))))
          (if (> (* (+ (field ht Size) empty) 2) (field (field ht RowTable) Capacity))
            (block
              (if (! (call agr_reindex (field ht RowTable) (field ht RowArena)
                           (+ (field ht Size) k)))
                (block (return #f)))
              (if first
                (block (call rt_seek_all (field ht RowTable) (field ht RowScratch) k))
                (block
                  (call agr_rehome_work (field ht RowTable) (field ht RowScratch) k)
                  (call rt_seek_work (field ht RowTable) (field ht RowScratch) k)))))
          (var entries = (field (field ht RowTable) Entries))
          (var mask = (field (field ht RowTable) Mask))
          (var m = 0)
          (= q 0)
          (while (< q k)
            (block
              (var j = q)
              (if (! first) (block (= j (index work q))))
              (var off = (index offsets j))
              (var key = (cast (+ keys (* j key_stride)) <ptr Key>))
              (var e = (index entries (cast off i64)))
              (if (== e (: 0 u64))
                (block
                  (var row = (call ra_new_row (field ht RowArena)))
                  (if (== (cast row i64) 0) (block (return #f)))
                  (var words = (cast row <ptr u64>))
                  (= words [0] (index hashes j))
                  (var row_key = (cast (+ (cast row i64) key_offset) <ptr Key>))
                  (= row_key [0] (index key 0))
                  (var states = (cast (+ (cast row i64) state_offset) <ptr i64>))
                  (var w = 0)
                  (while (< w num_aggs)
                    (block
                      (= states [w] 0)
                      (= w (+ w 1))))
                  (= entries [(cast off i64)] (call rt_entry (index salts j) row))
                  (field_assign ht Size (+ (field ht Size) 1))
                  (= found [j] (| (cast (cast row i64) u64) (: 1 u64))))
                (block
                  (var row = (call rt_entry_row e))
                  (var matched = #f)
                  (if (== (>> e (: 48 u64)) (index salts j))
                    (block
                      (var row_key = (cast (+ (cast row i64) key_offset) <ptr Key>))
                      (= matched (call rh_key_equal (index row_key 0) (index key 0)))))
                  (if matched
                    (block (= found [j] (cast (cast row i64) u64)))
                    (block
                      (= offsets [j] (& (+ off (: 1 u64)) mask))
                      (= work [m] j)
                      (= m (+ m 1))))))
              (= q (+ q 1))))
          (= k m)
          (= first #f)
          (if (> k 0)
            (block (call rt_seek_work (field ht RowTable) (field ht RowScratch) k)))))
      (return #t)))

  ;; Moves the groups of a swiss table to the row layout in their dense order
  ;; and frees the swiss storage. A swiss state sits at the probe slot of its
  ;; group (SlotId maps the group to it). The entries are sized for twice the
  ;; groups, as the swiss table was about to double, or for RowExpectedGroups
  ;; when the caller expects more, so that the entries need not grow again.
  (fun agr_from_swiss [Key] ((var ht <ref HashTable>) (var key_offset i64)
                             (var state_offset i64) (var row_bytes i64)
                             (var block_rows i64) (var witness <ptr Key>)) -> bool
    (block
      (var size = (field ht Size))
      (var num_aggs = (field ht NumAggs))
      (var groups = (* size 2))
      (if (> (field ht RowExpectedGroups) groups)
        (block (= groups (field ht RowExpectedGroups))))
      (call ra_init (field ht RowArena) row_bytes block_rows)
      (if (! (call rt_init (field ht RowTable) groups)) (block (return #f)))
      (var group_keys = (cast (field ht GroupKeys) <ptr Key>))
      (var slot_ids = (field ht SlotId))
      (var buffers = (field ht AggBuffers))
      (var g = 0)
      (while (< g size)
        (block
          (var row = (call ra_new_row (field ht RowArena)))
          (if (== (cast row i64) 0) (block (return #f)))
          (var key = (index group_keys g))
          (var hash = (cast (call rh_hash key) u64))
          (var words = (cast row <ptr u64>))
          (= words [0] hash)
          (var row_key = (cast (+ (cast row i64) key_offset) <ptr Key>))
          (= row_key [0] key)
          (var states = (cast (+ (cast row i64) state_offset) <ptr i64>))
          (var slot = (index slot_ids g))
          (var a = 0)
          (while (< a num_aggs)
            (block
              (var buffer = (index buffers a))
              (= states [a] (index buffer slot))
              (= a (+ a 1))))
          (call agr_link (field ht RowTable) row hash)
          (= g (+ g 1))))
      (call aht_destroy ht)
      (field_assign ht Size size)
      (field_assign ht NumAggs num_aggs)
      (return #t)))
)
