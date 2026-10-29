(block
  (pragma language overloads)

  ;; Row join table: the build side of an equi-join stored as rows of the
  ;; row table (kernel/row_table.oz), an arena indexed by a table of 8-byte
  ;; entries (16-bit salt, 48-bit row pointer). A probe touches one entry and,
  ;; on a salt match, one row, so a hit costs about two cache lines and a miss
  ;; one.
  ;;
  ;; It serves inner joins and the LEFT / RIGHT joins that build their
  ;; null-supplying side. Such a join streams its preserved side: a probe row
  ;; that matches nothing is emitted once with a null row pointer, which the
  ;; gathers read as NULL build columns.
  ;;
  ;; It also serves the SEMI / ANTI joins that build their preserved left side.
  ;; Their rows carry a mark byte: a probe marks the rows it matches and emits
  ;; nothing, and once the probe side ends the arena is scanned for the marked
  ;; rows (SEMI) or the unmarked ones (ANTI). A row with a NULL key matches
  ;; nothing, so the arena keeps only rows with a valid key; an ANTI join keeps
  ;; its NULL-key rows in a second arena that no table indexes.
  ;;
  ;; Row layout, fixed per join and generated in join_gen.cpp:
  ;;   [0, 8)        hash of the key while the arena is built, then the pointer
  ;;                 to the next row with an equal key (0 ends the chain);
  ;;   [8, 8 + key)  the LookupKey struct of the row;
  ;;   then one validity byte per nullable payload column, the mark byte of a
  ;;   SEMI / ANTI build side, and the payload values at their natural
  ;;   alignment; strings are StringView handles that point into the retained
  ;;   input batch, so string bytes are not copied.
  ;;
  ;; Rows with equal keys are chained through their first word, so every row
  ;; of a chain matches once its head matched. The build links rows from the
  ;; last to the first, so a chain lists them in build order, like the
  ;; symmetric table's buckets.
  ;;
  ;; A join on one integer key whose build keys turn out unique and within
  ;; 2^20 of each other gets a direct index instead of the salted table (see
  ;; jr_build_direct below).
  ;;
  ;; C++ (qdb/exec/row_join_exec.cpp) owns the zeroed RowArena / RowTable /
  ;; RowScratch structs and the retained input batches.

  ;; ---- chains of rows with equal keys ----

  (fun jr_row_next ((var row <ptr u8>)) -> <ptr u8>
    (attrs inline)
    (block
      (var words = (cast row <ptr u64>))
      (return (cast (cast (index words 0) i64) <ptr u8>))))

  (fun jr_row_set_next ((var row <ptr u8>) (var next <ptr u8>))
    (attrs inline)
    (block
      (var words = (cast row <ptr u64>))
      (= words [0] (cast (cast next i64) u64))))

  ;; Stores `row` at `off`, chained in front of `head` (null for a new key,
  ;; which Keys counts).
  (fun jrt_link ((var table <ref RowTable>) (var off u64) (var salt u64)
                 (var row <ptr u8>) (var head <ptr u8>))
    (block
      (call jr_row_set_next row head)
      (if (== (cast head i64) 0)
        (block (field_assign table Keys (+ (field table Keys) 1))))
      (var entries = (field table Entries))
      (= entries [(cast off i64)] (call rt_entry salt row))))

  ;; ---- build and probe, generic over the join's LookupKey ----
  ;; The key struct of a build row sits at key_offset. rh_key_equal is the
  ;; join's overload for that key; witness only names the key type.

  ;; Sizes the table and links every arena row into it, from the last row to
  ;; the first: an empty entry starts a new key, a salt match with an equal key
  ;; chains the row in front of that key's rows, anything else moves to the
  ;; next entry.
  (fun jr_build_rows [Key] ((var table <ref RowTable>) (var arena <ref RowArena>)
                            (var key_offset i64) (var witness <ptr Key>)) -> bool
    (block
      (if (! (call rt_init table (field arena Rows))) (block (return #f)))
      (var blocks = (field arena Blocks))
      (var count = (field arena BlockCount))
      (var row_bytes = (field arena RowBytes))
      (var shift = (cast (field table Shift) u64))
      (var mask = (field table Mask))
      (var b = (- count 1))
      (while (>= b 0)
        (block
          (var rows_in_block = (field arena BlockRows))
          (if (== b (- count 1)) (block (= rows_in_block (field arena CurRows))))
          (var base = (cast (index blocks b) i64))
          (var k = (- rows_in_block 1))
          (while (>= k 0)
            (block
              (var row = (cast (+ base (* k row_bytes)) <ptr u8>))
              (var h = (call rt_mix (call rt_row_hash row)))
              (var salt = (call rt_salt h))
              (var off = (: 0 u64))
              (var e = (call rt_seek table (>> h shift) salt off))
              (var row_key = (cast (+ (cast row i64) key_offset) <ptr Key>))
              (var done = #f)
              (while (! done)
                (block
                  (if (== e (: 0 u64))
                    (block
                      (call jrt_link table off salt row (cast 0 <ptr u8>))
                      (= done #t))
                    (block
                      (var head = (call rt_entry_row e))
                      (var head_key = (cast (+ (cast head i64) key_offset) <ptr Key>))
                      (if (call rh_key_equal (index head_key 0) (index row_key 0))
                        (block
                          (call jrt_link table off salt row head)
                          (= done #t))
                        (block
                          (= off (& (+ off (: 1 u64)) mask))
                          (= e (call rt_seek table off salt off))))))))
              (= k (- k 1))))
          (= b (- b 1))))
      (return #t)))

  ;; Emits pair (stream row `row`, `build`) in (left, right) order; a null
  ;; `build` pads an unmatched probe row of an outer join.
  (fun jr_push_pair ((var pairs <ref PairBuffer>) (var row i64) (var build <ptr u8>)
                     (var probe_is_left bool)) -> bool
    (attrs inline)
    (block
      ;; A stream row id: batch index -1, row `row`.
      (var stream_id = (+ (<< -1 32) row))
      (if probe_is_left
        (block (return (call pb_push pairs stream_id (cast build i64)))))
      (return (call pb_push pairs (cast build i64) stream_id))))

  ;; Marks the rows of the chain that starts at `row`. A probe marks a whole
  ;; chain at once, so the walk stops at the first marked row: a chain matched
  ;; before costs one byte read.
  (fun jr_mark_chain ((var row <ptr u8>) (var mark_offset i64))
    (attrs inline)
    (block
      (var cur = row)
      (while (&& (!= (cast cur i64) 0) (== (index cur mark_offset) (: 0 u8)))
        (block
          (= cur [mark_offset] (: 1 u8))
          (= cur (call jr_row_next cur))))))

  ;; Key pass of the probe over Entries[0, count), after rt_seek_all. Keys[j]
  ;; is the key of probe row Rows[j]. A match emits the whole chain, or marks
  ;; it when mark_offset is not negative; a salt collision advances the row's
  ;; offset and seeks again, until every row reached an empty entry. A row that
  ;; reaches one without a match is emitted with a null row pointer when
  ;; emit_unmatched is set. Pairs are (stream row id, row pointer) in
  ;; (left, right) order.
  (fun jr_probe_candidates [Key] ((var table <ref RowTable>) (var s <ref RowScratch>)
                                  (var pairs <ref PairBuffer>) (var count i64)
                                  (var key_offset i64) (var probe_is_left bool)
                                  (var emit_unmatched bool) (var mark_offset i64)
                                  (var witness <ptr Key>)) -> bool
    (block
      (var keys = (cast (field s Keys) <ptr Key>))
      (var rows = (field s Rows))
      (var work = (field s Work))
      (var entries = (field s Entries))
      (var offsets = (field s Offsets))
      (var mask = (field table Mask))
      (var k = count)
      (var first = #t)
      (while (> k 0)
        (block
          (var m = 0)
          (var q = 0)
          (while (< q k)
            (block
              (var j = q)
              (if (! first) (block (= j (index work q))))
              (var e = (index entries j))
              (if (!= e (: 0 u64))
                (block
                  (var row = (call rt_entry_row e))
                  (var row_key = (cast (+ (cast row i64) key_offset) <ptr Key>))
                  (if (call rh_key_equal (index row_key 0) (index keys j))
                    (block
                      (if (>= mark_offset 0)
                        (block (call jr_mark_chain row mark_offset))
                        (block
                          (var cur = row)
                          (while (!= (cast cur i64) 0)
                            (block
                              (if (! (call jr_push_pair pairs (index rows j) cur probe_is_left))
                                (block (return #f)))
                              (= cur (call jr_row_next cur)))))))
                    (block
                      (= offsets [j] (& (+ (index offsets j) (: 1 u64)) mask))
                      (= work [m] j)
                      (= m (+ m 1)))))
                (block
                  (if (&& emit_unmatched
                          (! (call jr_push_pair pairs (index rows j) (cast 0 <ptr u8>)
                                   probe_is_left)))
                    (block (return #f)))))
              (= q (+ q 1))))
          (= k m)
          (= first #f)
          (if (> k 0) (block (call rt_seek_work table s k)))))
      (return #t)))

  ;; Pushes (row pointer, -1) for every arena row whose mark is `marked`, in
  ;; build order: the matched rows of a SEMI join, the unmatched ones of an
  ;; ANTI join. Such a join builds its left side and emits no probe columns.
  (fun jr_emit_by_mark ((var arena <ref RowArena>) (var pairs <ref PairBuffer>)
                        (var mark_offset i64) (var marked bool)) -> bool
    (block
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
              (var emit = (!= (index bytes mark_offset) (: 0 u8)))
              (if (! marked) (block (= emit (! emit))))
              (if (&& emit (! (call pb_push pairs row -1)))
                (block (return #f)))
              (= row (+ row row_bytes))))
          (= b (+ b 1))))
      (return #t)))

  (fun jr_block_rows ((var arena <ref RowArena>) (var b i64)) -> i64
    (attrs inline)
    (block
      (if (== b (- (field arena BlockCount) 1)) (block (return (field arena CurRows))))
      (return (field arena BlockRows))))

  ;; ---- direct index: one unique integer key in a dense range ----
  ;; A perfect hash: when the keys of the complete build side are unique and
  ;; max - min <= 2^20, Direct holds the row of key k at k - DirectMin and
  ;; null for an absent key. A probe needs no hash, salt or key comparison: a
  ;; key outside the range costs one bounds check, a key inside it one load.
  ;; A slot points to the arena row. Entries stays null; Capacity is the slot
  ;; count, so the table reports its real size.

  ;; The key of `row` as an i64; Value is the key's integer type.
  (fun jr_row_int [Value] ((var row <ptr u8>) (var value_offset i64)
                           (var witness <ptr Value>)) -> i64
    (attrs inline)
    (block
      (var slot = (cast (+ (cast row i64) value_offset) <ptr Value>))
      (return (cast (index slot 0) i64))))

  ;; Builds the direct index over the arena when its keys allow one, reading
  ;; each key at value_offset of the row. Returns #f and leaves the table
  ;; untouched when the keys span more than 2^20, repeat, or the index cannot
  ;; be allocated; the caller then builds the salted table.
  (fun jr_build_direct [Value] ((var table <ref RowTable>) (var arena <ref RowArena>)
                                (var value_offset i64) (var witness <ptr Value>)) -> bool
    (block
      (var max_span = (: 1048576 u64))
      (var rows = (field arena Rows))
      ;; More rows than possible slots must repeat a key.
      (if (|| (== rows 0) (> (cast rows u64) (+ max_span (: 1 u64)))) (block (return #f)))
      (var blocks = (field arena Blocks))
      (var count = (field arena BlockCount))
      (var row_bytes = (field arena RowBytes))
      ;; Pass 1: the key range, given up after the first block that widens it
      ;; past the limit. The span is unsigned, so it cannot overflow.
      (var lo = (call jr_row_int (index blocks 0) value_offset witness))
      (var hi = lo)
      (var b = 0)
      (while (< b count)
        (block
          (var row = (cast (index blocks b) i64))
          (var end = (+ row (* (call jr_block_rows arena b) row_bytes)))
          (while (< row end)
            (block
              (var v = (call jr_row_int (cast row <ptr u8>) value_offset witness))
              (if (< v lo) (block (= lo v)))
              (if (> v hi) (block (= hi v)))
              (= row (+ row row_bytes))))
          (if (> (- (cast hi u64) (cast lo u64)) max_span) (block (return #f)))
          (= b (+ b 1))))
      (var slots = (+ (cast (- (cast hi u64) (cast lo u64)) i64) 1))
      (if (> rows slots) (block (return #f)))
      ;; Pass 2: a bitmap of the range finds a repeated key before the index
      ;; is allocated, so a failed attempt costs one bit per slot, not eight
      ;; bytes.
      (var words = (>> (+ slots 63) 6))
      (var seen = (cast (call qdb_alloc (* words 8)) <ptr u64>))
      (if (== (cast seen i64) 0) (block (return #f)))
      (var w = 0)
      (while (< w words)
        (block
          (= seen [w] (: 0 u64))
          (= w (+ w 1))))
      (= b 0)
      (while (< b count)
        (block
          (var row = (cast (index blocks b) i64))
          (var end = (+ row (* (call jr_block_rows arena b) row_bytes)))
          (while (< row end)
            (block
              (var off = (- (cast (call jr_row_int (cast row <ptr u8>) value_offset witness) u64)
                            (cast lo u64)))
              (var word = (cast (>> off (: 6 u64)) i64))
              (var bit = (<< (: 1 u64) (& off (: 63 u64))))
              (if (!= (& (index seen word) bit) (: 0 u64))
                (block
                  (call qdb_free (cast seen <ptr i8>))
                  (return #f)))
              (= seen [word] (| (index seen word) bit))
              (= row (+ row row_bytes))))
          (= b (+ b 1))))
      (call qdb_free (cast seen <ptr i8>))
      ;; Pass 3: the row of every key into its slot.
      (var direct = (cast (call qdb_alloc (* slots 8)) <ptr <ptr u8>>))
      (if (== (cast direct i64) 0) (block (return #f)))
      (var i = 0)
      (while (< i slots)
        (block
          (= direct [i] (cast 0 <ptr u8>))
          (= i (+ i 1))))
      (= b 0)
      (while (< b count)
        (block
          (var row = (cast (index blocks b) i64))
          (var end = (+ row (* (call jr_block_rows arena b) row_bytes)))
          (while (< row end)
            (block
              (var v = (call jr_row_int (cast row <ptr u8>) value_offset witness))
              (= direct [(cast (- (cast v u64) (cast lo u64)) i64)] (cast row <ptr u8>))
              (= row (+ row row_bytes))))
          (= b (+ b 1))))
      (field_assign table Direct direct)
      (field_assign table DirectMin lo)
      (field_assign table DirectSlots (cast slots u64))
      (field_assign table Capacity slots)
      (field_assign table Rows rows)
      (field_assign table Keys rows)
      (return #t)))

  ;; Probes one row with key `key` (an i64 of the key's integer type). A
  ;; present key emits its pair, in (left, right) order, or marks its build row
  ;; when mark_offset is not negative; an absent key pads the row with a null
  ;; build row when emit_unmatched is set.
  (fun jr_direct_probe ((var table <ref RowTable>) (var pairs <ref PairBuffer>)
                        (var key i64) (var probe_row i64) (var probe_is_left bool)
                        (var emit_unmatched bool) (var mark_offset i64)) -> bool
    (attrs inline)
    (block
      (var off = (- (cast key u64) (cast (field table DirectMin) u64)))
      (var direct = (field table Direct))
      (var row = 0)
      (if (< off (field table DirectSlots))
        (block (= row (cast (index direct (cast off i64)) i64))))
      (if (== row 0)
        (block
          (if emit_unmatched
            (block (return (call jr_push_pair pairs probe_row (cast 0 <ptr u8>) probe_is_left))))
          (return #t)))
      ;; A slot holds the only row of its key, and jr_build_direct links no
      ;; chains (the first word keeps the hash), so the row is marked here
      ;; rather than by jr_mark_chain.
      (if (>= mark_offset 0)
        (block
          (var build = (cast row <ptr u8>))
          (= build [mark_offset] (: 1 u8))
          (return #t)))
      (return (call jr_push_pair pairs probe_row (cast row <ptr u8>) probe_is_left))))

  ;; ---- payload: column -> row ----

  (fun jr_col_valid ((var col <ref TColumn>) (var row i64)) -> bool
    (attrs inline)
    (block
      (return (|| (== (cast (field col Mask) i64) 0)
                  (call bitoff (field col Mask) row (cast (field col MaskBitOffset) i64))))))

  (fun jr_store_valid ((var dst <ptr u8>) (var valid_offset i64) (var valid bool))
    (attrs inline)
    (block
      (if (>= valid_offset 0)
        (block
          (var byte = (: 0 u8))
          (if valid (block (= byte (: 1 u8))))
          (= dst [valid_offset] byte)))))

  (fun jr_store_fixed [Value] ((var dst <ptr u8>) (var offset i64) (var valid_offset i64)
                               (var col <ref TColumn>) (var row i64) (var witness <ptr Value>))
    (block
      (var valid = (call jr_col_valid col row))
      (call jr_store_valid dst valid_offset valid)
      (var value = (cast (: 0 i64) Value))
      (if valid
        (block
          (var src = (cast (cast (field col Data) i64) <ptr Value>))
          (= value (index src row))))
      (var slot = (cast (+ (cast dst i64) offset) <ptr Value>))
      (= slot [0] value)))

  (fun jr_store_bool ((var dst <ptr u8>) (var offset i64) (var valid_offset i64)
                      (var col <ref TColumn>) (var row i64))
    (block
      (var valid = (call jr_col_valid col row))
      (call jr_store_valid dst valid_offset valid)
      (var byte = (: 0 u8))
      (if (&& valid (call bitoff (cast (cast (field col Data) i64) <ptr u8>) row
                          (cast (field col DataBitOffset) i64)))
        (block (= byte (: 1 u8))))
      (= dst [offset] byte)))

  (fun jr_store_binint ((var dst <ptr u8>) (var offset i64) (var valid_offset i64)
                        (var col <ref TColumn>) (var row i64))
    (block
      (var valid = (call jr_col_valid col row))
      (call jr_store_valid dst valid_offset valid)
      (var value = (cast (struct ((V (: 0 i128)))) BinInt))
      (if valid
        (block
          (var src = (cast (cast (field col Data) i64) <ptr BinInt>))
          (= value (index src row))))
      (var slot = (cast (+ (cast dst i64) offset) <ptr BinInt>))
      (= slot [0] value)))

  ;; Stores a StringView into the column's bytes; the batch stays retained.
  (fun jr_store_string ((var dst <ptr u8>) (var offset i64) (var valid_offset i64)
                        (var col <ref TColumn>) (var row i64))
    (block
      (var valid = (call jr_col_valid col row))
      (call jr_store_valid dst valid_offset valid)
      (var begin = 0)
      (var len = 0)
      (if valid
        (block
          (if (== (cast (field col OffsetWidth) i64) 4)
            (block
              (var src_offsets = (cast (cast (field col Offsets) i64) <ptr i32>))
              (var base = (cast (index src_offsets 0) i64))
              (= begin (- (cast (index src_offsets row) i64) base))
              (= len (- (cast (index src_offsets (+ row 1)) i64)
                        (cast (index src_offsets row) i64))))
            (block
              (var src_offsets = (cast (cast (field col Offsets) i64) <ptr i64>))
              (var base = (index src_offsets 0))
              (= begin (- (index src_offsets row) base))
              (= len (- (index src_offsets (+ row 1)) (index src_offsets row)))))))
      (var slot = (cast (+ (cast dst i64) offset) <ptr i64>))
      (= slot [0] (+ (cast (field col Data) i64) begin))
      (= slot [1] len)))

  ;; ---- output: row -> column ----
  ;; Same output format as jt_materialize_*_column (materialize.oz); the pair
  ;; side `pair_offset` holds the row pointer as an i64, null for the build side
  ;; of an unmatched outer probe row, whose columns are all NULL.

  (fun jr_pair_row ((var pair_data <ptr i64>) (var start i64) (var i i64)
                    (var pair_offset i64)) -> <ptr u8>
    (attrs inline)
    (block
      (return (cast (index pair_data (+ (* (+ start i) 2) pair_offset)) <ptr u8>))))

  (fun jr_row_valid ((var row <ptr u8>) (var valid_offset i64)) -> bool
    (attrs inline)
    (block
      (return (&& (!= (cast row i64) 0)
                  (|| (< valid_offset 0) (!= (index row valid_offset) (: 0 u8)))))))

  (fun jr_gather_fixed_column [Value]
       ((var pairs <ref PairBuffer>) (var start i64) (var n i64) (var pair_offset i64)
        (var row_offset i64) (var valid_offset i64)
        (var out_col <ref TColumn>) (var out <ref TRowSet>)
        (var data_owner_idx i64) (var mask_owner_idx i64)
        (var value_bytes i64) (var witness <ptr Value>))
    (block
      (var data = (cast (call qdb_alloc (* n value_bytes)) <ptr Value>))
      (var mask = (cast (call qdb_alloc (call jm_mask_bytes n)) <ptr u8>))
      (call jm_record_owner out data_owner_idx (cast data <ptr i8>))
      (call jm_record_owner out mask_owner_idx (cast mask <ptr i8>))
      (field_assign out_col Data (cast data <ptr i8>))
      (field_assign out_col DataBitOffset (: 0 i32))
      (field_assign out_col Mask mask)
      (field_assign out_col MaskBitOffset (: 0 i32))
      (field_assign out_col Offsets (cast 0 <ptr i64>))
      (field_assign out_col OffsetWidth (: 0 u8))
      (var pair_data = (field pairs Data))
      (var i = 0)
      (while (< i n)
        (block
          (var row = (call jr_pair_row pair_data start i pair_offset))
          (var valid = (call jr_row_valid row valid_offset))
          (var value = (cast (: 0 i64) Value))
          (if valid
            (block
              (var slot = (cast (+ (cast row i64) row_offset) <ptr Value>))
              (= value (index slot 0))))
          (= data [i] value)
          (call qdb_bitmap_set_valid mask i valid)
          (= i (+ i 1))))))

  (fun jr_gather_bool_column
       ((var pairs <ref PairBuffer>) (var start i64) (var n i64) (var pair_offset i64)
        (var row_offset i64) (var valid_offset i64)
        (var out_col <ref TColumn>) (var out <ref TRowSet>)
        (var data_owner_idx i64) (var mask_owner_idx i64))
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
      (var i = 0)
      (while (< i n)
        (block
          (var row = (call jr_pair_row pair_data start i pair_offset))
          (var valid = (call jr_row_valid row valid_offset))
          (var value = (&& valid (!= (index row row_offset) (: 0 u8))))
          (call qdb_bitmap_set_valid data i value)
          (call qdb_bitmap_set_valid mask i valid)
          (= i (+ i 1))))))

  (fun jr_gather_binint_column
       ((var pairs <ref PairBuffer>) (var start i64) (var n i64) (var pair_offset i64)
        (var row_offset i64) (var valid_offset i64)
        (var out_col <ref TColumn>) (var out <ref TRowSet>)
        (var data_owner_idx i64) (var mask_owner_idx i64))
    (block
      (var data = (cast (call qdb_alloc (* n 16)) <ptr BinInt>))
      (var mask = (cast (call qdb_alloc (call jm_mask_bytes n)) <ptr u8>))
      (call jm_record_owner out data_owner_idx (cast data <ptr i8>))
      (call jm_record_owner out mask_owner_idx (cast mask <ptr i8>))
      (field_assign out_col Data (cast data <ptr i8>))
      (field_assign out_col DataBitOffset (: 0 i32))
      (field_assign out_col Mask mask)
      (field_assign out_col MaskBitOffset (: 0 i32))
      (field_assign out_col Offsets (cast 0 <ptr i64>))
      (field_assign out_col OffsetWidth (: 0 u8))
      (var pair_data = (field pairs Data))
      (var zero = (cast (struct ((V (: 0 i128)))) BinInt))
      (var i = 0)
      (while (< i n)
        (block
          (var row = (call jr_pair_row pair_data start i pair_offset))
          (var valid = (call jr_row_valid row valid_offset))
          (var value = zero)
          (if valid
            (block
              (var slot = (cast (+ (cast row i64) row_offset) <ptr BinInt>))
              (= value (index slot 0))))
          (= data [i] value)
          (call qdb_bitmap_set_valid mask i valid)
          (= i (+ i 1))))))

  (fun jr_gather_string_column
       ((var pairs <ref PairBuffer>) (var start i64) (var n i64) (var pair_offset i64)
        (var row_offset i64) (var valid_offset i64)
        (var out_col <ref TColumn>) (var out <ref TRowSet>)
        (var offsets_owner_idx i64) (var data_owner_idx i64) (var mask_owner_idx i64))
    (block
      (var offsets = (cast (call qdb_alloc (* (+ n 1) 8)) <ptr i64>))
      (var mask = (cast (call qdb_alloc (call jm_mask_bytes n)) <ptr u8>))
      (call jm_record_owner out offsets_owner_idx (cast offsets <ptr i8>))
      (call jm_record_owner out mask_owner_idx (cast mask <ptr i8>))
      (= offsets [0] 0)
      (var pair_data = (field pairs Data))
      (var i = 0)
      (while (< i n)
        (block
          (var row = (call jr_pair_row pair_data start i pair_offset))
          (var len = 0)
          (if (call jr_row_valid row valid_offset)
            (block
              (var view = (cast (+ (cast row i64) row_offset) <ptr i64>))
              (= len (index view 1))))
          (= offsets [(+ i 1)] (+ (index offsets i) len))
          (= i (+ i 1))))
      (var data = (cast (call qdb_alloc (index offsets n)) <ptr u8>))
      (call jm_record_owner out data_owner_idx (cast data <ptr i8>))
      (field_assign out_col Data (cast data <ptr i8>))
      (field_assign out_col DataBitOffset (: 0 i32))
      (field_assign out_col Mask mask)
      (field_assign out_col MaskBitOffset (: 0 i32))
      (field_assign out_col Offsets offsets)
      (field_assign out_col OffsetWidth (: 8 u8))
      (= i 0)
      (while (< i n)
        (block
          (var row = (call jr_pair_row pair_data start i pair_offset))
          (var valid = (call jr_row_valid row valid_offset))
          (if valid
            (block
              (var view = (cast (+ (cast row i64) row_offset) <ptr i64>))
              (var src = (cast (index view 0) <ptr u8>))
              (var len = (index view 1))
              (var dst = (cast (+ (cast data i64) (index offsets i)) <ptr u8>))
              (var b = 0)
              (while (< b len)
                (block
                  (= dst [b] (index src b))
                  (= b (+ b 1))))))
          (call qdb_bitmap_set_valid mask i valid)
          (= i (+ i 1))))))
)
