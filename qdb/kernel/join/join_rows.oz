(block
  (pragma language overloads)

  ;; Row join table: the build side of an inner equi-join stored as rows in an
  ;; arena, indexed by a table of 8-byte entries (16-bit salt, 48-bit row
  ;; pointer). A probe touches one entry and, on a salt match, one row, so a
  ;; hit costs about two cache lines and a miss one. The layout follows
  ;; DuckDB's JoinHashTable (join_hashtable.cpp, ht_entry.hpp).
  ;;
  ;; Row layout, fixed per join and generated in join_gen.cpp:
  ;;   [0, 8)        hash of the key while the arena is built, then the pointer
  ;;                 to the next row with an equal key (0 ends the chain);
  ;;   [8, 8 + key)  the LookupKey struct of the row;
  ;;   then one validity byte per nullable payload column and the payload
  ;;   values at their natural alignment; strings are StringView handles that
  ;;   point into the retained input batch, so string bytes are not copied.
  ;;
  ;; Table index = mixed hash >> Shift (the top bits), salt = bits 16..31 of
  ;; the mixed hash (jrt_mix).
  ;; The hash shuffle partitions a join by the low bits, so a partition's table
  ;; must not index by them; the top bits stay uniform inside a partition.
  ;; Linear probing stops at an empty entry or a salt match; only then is the
  ;; row's key compared. Rows with equal keys are chained through their first
  ;; word, so every row of a chain matches once its head matched.
  ;;
  ;; C++ (qdb/exec/row_join_exec.cpp) owns the zeroed JoinArena / JoinTable /
  ;; JoinScratch structs and the retained input batches; every buffer they point
  ;; to is qdb_alloc'd here and freed by the destroy functions below.

  ;; ---- arena of build rows ----

  (fun ja_init ((var arena <ref JoinArena>) (var row_bytes i64) (var block_rows i64))
    (block
      (field_assign arena Blocks (cast 0 <ptr <ptr u8>>))
      (field_assign arena BlockCount 0)
      (field_assign arena BlockCapacity 0)
      (field_assign arena BlockRows block_rows)
      (field_assign arena RowBytes row_bytes)
      (field_assign arena Rows 0)
      ;; A full current block makes the first append allocate one.
      (field_assign arena CurRows block_rows)))

  ;; Returns the next row slot, or null when allocation failed.
  (fun ja_new_row ((var arena <ref JoinArena>)) -> <ptr u8>
    (block
      (var cur = (field arena CurRows))
      (if (== cur (field arena BlockRows))
        (block
          (var count = (field arena BlockCount))
          (var capacity = (field arena BlockCapacity))
          (if (== count capacity)
            (block
              (var new_cap = 16)
              (if (!= capacity 0) (block (= new_cap (* capacity 2))))
              (var grown =
                (cast (call qdb_realloc (cast (field arena Blocks) <ptr i8>)
                        (* count 8) (* new_cap 8)) <ptr <ptr u8>>))
              (if (== (cast grown i64) 0) (block (return (cast 0 <ptr u8>))))
              (field_assign arena Blocks grown)
              (field_assign arena BlockCapacity new_cap)))
          (var block =
            (cast (call qdb_alloc (* (field arena BlockRows) (field arena RowBytes))) <ptr u8>))
          (if (== (cast block i64) 0) (block (return block)))
          (var blocks = (field arena Blocks))
          (= blocks [count] block)
          (field_assign arena BlockCount (+ count 1))
          (= cur 0)))
      (var current = (field arena Blocks))
      (var base = (index current (- (field arena BlockCount) 1)))
      (field_assign arena CurRows (+ cur 1))
      (field_assign arena Rows (+ (field arena Rows) 1))
      (return (cast (+ (cast base i64) (* cur (field arena RowBytes))) <ptr u8>))))

  (fun ja_destroy ((var arena <ref JoinArena>))
    (block
      (var blocks = (field arena Blocks))
      (if (!= (cast blocks i64) 0)
        (block
          (var i = 0)
          (while (< i (field arena BlockCount))
            (block
              (call qdb_free (cast (index blocks i) <ptr i8>))
              (= i (+ i 1))))
          (call qdb_free (cast blocks <ptr i8>))))
      (field_assign arena Blocks (cast 0 <ptr <ptr u8>>))
      (field_assign arena BlockCount 0)
      (field_assign arena BlockCapacity 0)
      (field_assign arena Rows 0)
      (field_assign arena CurRows 0)))

  ;; ---- rows ----

  (fun jr_row_hash ((var row <ptr u8>)) -> u64
    (block
      (var words = (cast row <ptr u64>))
      (return (index words 0))))

  (fun jr_row_next ((var row <ptr u8>)) -> <ptr u8>
    (block
      (var words = (cast row <ptr u64>))
      (return (cast (cast (index words 0) i64) <ptr u8>))))

  (fun jr_row_set_next ((var row <ptr u8>) (var next <ptr u8>))
    (block
      (var words = (cast row <ptr u64>))
      (= words [0] (cast (cast next i64) u64))))

  ;; ---- entry table ----

  (fun jrt_init ((var table <ref JoinTable>) (var rows i64)) -> bool
    (block
      ;; Load factor at most 1/2, as DuckDB's DEFAULT_LOAD_FACTOR; the minimum
      ;; keeps the shift well defined and the table trivially small.
      (var capacity = 1024)
      (while (< capacity (* rows 2)) (block (= capacity (* capacity 2))))
      (var shift = 64)
      (var c = capacity)
      (while (> c 1)
        (block
          (= c (>> c 1))
          (= shift (- shift 1))))
      (var entries = (cast (call qdb_alloc (* capacity 8)) <ptr u64>))
      (if (== (cast entries i64) 0) (block (return #f)))
      (var i = 0)
      (while (< i capacity)
        (block
          (= entries [i] (: 0 u64))
          (= i (+ i 1))))
      (field_assign table Entries entries)
      (field_assign table Capacity capacity)
      (field_assign table Mask (cast (- capacity 1) u64))
      (field_assign table Shift shift)
      (field_assign table Rows rows)
      (field_assign table ChainsLongerThanOne 0)
      (return #t)))

  (fun jrt_destroy ((var table <ref JoinTable>))
    (block
      (var entries = (field table Entries))
      (if (!= (cast entries i64) 0) (block (call qdb_free (cast entries <ptr i8>))))
      (field_assign table Entries (cast 0 <ptr u64>))
      (field_assign table Capacity 0)
      (field_assign table Rows 0)))

  ;; The table takes its index and salt from the top half of the hash, where a
  ;; key hash may carry little entropy: a string of up to 16 bytes hashes to
  ;; byte-swapped products, whose top 16 bits depend on four of its bytes. A
  ;; Fibonacci multiply spreads every bit of the hash over the top bits.
  (fun jrt_mix ((var hash u64)) -> u64
    (attrs inline)
    (block (return (* hash (: -7046029254386353131 u64)))))

  (fun jrt_salt ((var hash u64)) -> u64
    (attrs inline)
    (block (return (& (>> hash (: 16 u64)) (: 65535 u64)))))

  (fun jrt_entry ((var salt u64) (var row <ptr u8>)) -> u64
    (attrs inline)
    (block (return (| (<< salt (: 48 u64)) (cast (cast row i64) u64)))))

  (fun jrt_entry_row ((var entry u64)) -> <ptr u8>
    (attrs inline)
    (block (return (cast (cast (& entry (: 281474976710655 u64)) i64) <ptr u8>))))

  ;; Walks from `off` to the first entry that is empty or carries `salt`.
  ;; Returns that entry (0 when empty) and leaves its offset in out_off.
  (fun jrt_seek ((var table <ref JoinTable>) (var off u64) (var salt u64)
                 (var out_off <ref u64>)) -> u64
    (block
      (var entries = (field table Entries))
      (var mask = (field table Mask))
      (var e = (index entries (cast off i64)))
      (while (&& (!= e (: 0 u64)) (!= (>> e (: 48 u64)) salt))
        (block
          (= off (& (+ off (: 1 u64)) mask))
          (= e (index entries (cast off i64)))))
      (= out_off off)
      (return e)))

  ;; Stores `row` at `off`, chained in front of `head` (null for a new key).
  (fun jrt_link ((var table <ref JoinTable>) (var off u64) (var salt u64)
                 (var row <ptr u8>) (var head <ptr u8>))
    (block
      (call jr_row_set_next row head)
      (if (!= (cast head i64) 0)
        (block (field_assign table ChainsLongerThanOne 1)))
      (var entries = (field table Entries))
      (= entries [(cast off i64)] (call jrt_entry salt row))))

  ;; ---- probe scratch: one slot per selected probe row ----

  (fun jr_scratch_destroy ((var s <ref JoinScratch>))
    (block
      (if (!= (cast (field s Hashes) i64) 0)
        (block
          (call qdb_free (cast (field s Hashes) <ptr i8>))
          (call qdb_free (cast (field s Salts) <ptr i8>))
          (call qdb_free (cast (field s Offsets) <ptr i8>))
          (call qdb_free (cast (field s Entries) <ptr i8>))
          (call qdb_free (cast (field s Rows) <ptr i8>))
          (call qdb_free (cast (field s Work) <ptr i8>))
          (call qdb_free (cast (field s Keys) <ptr i8>))))
      (field_assign s Hashes (cast 0 <ptr u64>))
      (field_assign s Salts (cast 0 <ptr u64>))
      (field_assign s Offsets (cast 0 <ptr u64>))
      (field_assign s Entries (cast 0 <ptr u64>))
      (field_assign s Rows (cast 0 <ptr i64>))
      (field_assign s Work (cast 0 <ptr i64>))
      (field_assign s Keys (cast 0 <ptr u8>))
      (field_assign s Capacity 0)))

  ;; key_bytes is the size of the join's LookupKey struct.
  (fun jr_scratch_reserve ((var s <ref JoinScratch>) (var n i64) (var key_bytes i64)) -> bool
    (block
      (if (<= n (field s Capacity)) (block (return #t)))
      (call jr_scratch_destroy s)
      (var bytes = (* n 8))
      (field_assign s Hashes (cast (call qdb_alloc bytes) <ptr u64>))
      (field_assign s Salts (cast (call qdb_alloc bytes) <ptr u64>))
      (field_assign s Offsets (cast (call qdb_alloc bytes) <ptr u64>))
      (field_assign s Entries (cast (call qdb_alloc bytes) <ptr u64>))
      (field_assign s Rows (cast (call qdb_alloc bytes) <ptr i64>))
      (field_assign s Work (cast (call qdb_alloc bytes) <ptr i64>))
      (field_assign s Keys (cast (call qdb_alloc (* n key_bytes)) <ptr u8>))
      (if (|| (== (cast (field s Hashes) i64) 0)
              (|| (== (cast (field s Salts) i64) 0)
              (|| (== (cast (field s Offsets) i64) 0)
              (|| (== (cast (field s Entries) i64) 0)
              (|| (== (cast (field s Rows) i64) 0)
              (|| (== (cast (field s Work) i64) 0)
                  (== (cast (field s Keys) i64) 0)))))))
        (block (return #f)))
      (field_assign s Capacity n)
      (return #t)))

  ;; Pointer pass over Hashes[0, count): the entry for every probe row, its
  ;; salt and the offset it was found at. The rows are independent, so the
  ;; loop overlaps their cache misses.
  (fun jr_seek_all ((var table <ref JoinTable>) (var s <ref JoinScratch>) (var count i64))
    (block
      (var hashes = (field s Hashes))
      (var salts = (field s Salts))
      (var offsets = (field s Offsets))
      (var found = (field s Entries))
      (var shift = (cast (field table Shift) u64))
      (var j = 0)
      (while (< j count)
        (block
          (var h = (call jrt_mix (index hashes j)))
          (var salt = (call jrt_salt h))
          (var off = (: 0 u64))
          (var e = (call jrt_seek table (>> h shift) salt off))
          (= salts [j] salt)
          (= offsets [j] off)
          (= found [j] e)
          (= j (+ j 1))))))

  ;; Same for the rows in Work[0, count) whose keys did not match the entry
  ;; they were at; their Offsets were already advanced past it.
  (fun jr_seek_work ((var table <ref JoinTable>) (var s <ref JoinScratch>) (var count i64))
    (block
      (var salts = (field s Salts))
      (var offsets = (field s Offsets))
      (var found = (field s Entries))
      (var work = (field s Work))
      (var q = 0)
      (while (< q count)
        (block
          (var j = (index work q))
          (var off = (: 0 u64))
          (var e = (call jrt_seek table (index offsets j) (index salts j) off))
          (= offsets [j] off)
          (= found [j] e)
          (= q (+ q 1))))))

  ;; ---- build and probe, generic over the join's LookupKey ----
  ;; The key struct of a build row sits at key_offset. rh_key_equal is the
  ;; join's overload for that key; witness only names the key type.

  ;; Sizes the table and links every arena row into it: an empty entry starts
  ;; a new key, a salt match with an equal key chains the row in front of that
  ;; key's rows, anything else moves to the next entry.
  (fun jr_build_rows [Key] ((var table <ref JoinTable>) (var arena <ref JoinArena>)
                            (var key_offset i64) (var witness <ptr Key>)) -> bool
    (block
      (if (! (call jrt_init table (field arena Rows))) (block (return #f)))
      (var blocks = (field arena Blocks))
      (var count = (field arena BlockCount))
      (var row_bytes = (field arena RowBytes))
      (var shift = (cast (field table Shift) u64))
      (var mask = (field table Mask))
      (var b = 0)
      (while (< b count)
        (block
          (var rows_in_block = (field arena BlockRows))
          (if (== b (- count 1)) (block (= rows_in_block (field arena CurRows))))
          (var base = (cast (index blocks b) i64))
          (var k = 0)
          (while (< k rows_in_block)
            (block
              (var row = (cast (+ base (* k row_bytes)) <ptr u8>))
              (var h = (call jrt_mix (call jr_row_hash row)))
              (var salt = (call jrt_salt h))
              (var off = (: 0 u64))
              (var e = (call jrt_seek table (>> h shift) salt off))
              (var row_key = (cast (+ (cast row i64) key_offset) <ptr Key>))
              (var done = #f)
              (while (! done)
                (block
                  (if (== e (: 0 u64))
                    (block
                      (call jrt_link table off salt row (cast 0 <ptr u8>))
                      (= done #t))
                    (block
                      (var head = (call jrt_entry_row e))
                      (var head_key = (cast (+ (cast head i64) key_offset) <ptr Key>))
                      (if (call rh_key_equal (index head_key 0) (index row_key 0))
                        (block
                          (call jrt_link table off salt row head)
                          (= done #t))
                        (block
                          (= off (& (+ off (: 1 u64)) mask))
                          (= e (call jrt_seek table off salt off))))))))
              (= k (+ k 1))))
          (= b (+ b 1))))
      (return #t)))

  ;; Key pass of the probe over Entries[0, count), after jr_seek_all. Keys[j]
  ;; is the key of probe row Rows[j]. A match emits the whole chain; a salt
  ;; collision advances the row's offset and seeks again, until every row
  ;; reached an empty entry. Pairs are (stream row id, row pointer) in
  ;; (left, right) order.
  (fun jr_probe_candidates [Key] ((var table <ref JoinTable>) (var s <ref JoinScratch>)
                                  (var pairs <ref PairBuffer>) (var count i64)
                                  (var key_offset i64) (var probe_is_left bool)
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
                  (var row = (call jrt_entry_row e))
                  (var row_key = (cast (+ (cast row i64) key_offset) <ptr Key>))
                  (if (call rh_key_equal (index row_key 0) (index keys j))
                    (block
                      ;; A stream row id: batch index -1, row Rows[j].
                      (var stream_id = (+ (<< -1 32) (index rows j)))
                      (var cur = row)
                      (while (!= (cast cur i64) 0)
                        (block
                          (var pushed = #f)
                          (if probe_is_left
                            (block (= pushed (call pb_push pairs stream_id (cast cur i64))))
                            (block (= pushed (call pb_push pairs (cast cur i64) stream_id))))
                          (if (! pushed) (block (return #f)))
                          (= cur (call jr_row_next cur)))))
                    (block
                      (= offsets [j] (& (+ (index offsets j) (: 1 u64)) mask))
                      (= work [m] j)
                      (= m (+ m 1))))))
              (= q (+ q 1))))
          (= k m)
          (= first #f)
          (if (> k 0) (block (call jr_seek_work table s k)))))
      (return #t)))

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
  ;; side `pair_offset` holds the row pointer as an i64.

  (fun jr_pair_row ((var pair_data <ptr i64>) (var start i64) (var i i64)
                    (var pair_offset i64)) -> <ptr u8>
    (attrs inline)
    (block
      (return (cast (index pair_data (+ (* (+ start i) 2) pair_offset)) <ptr u8>))))

  (fun jr_row_valid ((var row <ptr u8>) (var valid_offset i64)) -> bool
    (attrs inline)
    (block
      (return (|| (< valid_offset 0) (!= (index row valid_offset) (: 0 u8))))))

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
