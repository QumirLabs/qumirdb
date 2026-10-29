(block
  (pragma language overloads)

  ;; Row table: rows in an arena of fixed-size blocks, indexed by a table of
  ;; 8-byte entries, each a 16-bit salt above a 48-bit row pointer. A lookup
  ;; touches one entry and, on a salt match, one row. The row join table
  ;; (join/join_rows.oz) and the row layout of the aggregate table
  ;; (aggregation/aggregation_rows.oz) are built on it; each lays out its own
  ;; rows after the first word, which holds the hash of the row's key when the
  ;; row is appended.
  ;;
  ;; Table index = mixed hash >> Shift (the top bits), salt = bits 16..31 of
  ;; the mixed hash (rt_mix). The hash shuffle partitions by the low bits, so a
  ;; partition's table must not index by them; the top bits stay uniform inside
  ;; a partition. Linear probing stops at an empty entry or a salt match; only
  ;; then is the row's key compared.
  ;;
  ;; A batch is looked up in passes over one scratch slot per row: the caller
  ;; fills the hashes and keys, rt_seek_all walks every row to its first
  ;; candidate entry, and the caller compares the keys there and sends the rows
  ;; that collided on through rt_seek_work. The rows of a pass are independent,
  ;; so their cache misses overlap.
  ;;
  ;; The owner keeps the zeroed RowArena / RowTable / RowScratch structs;
  ;; every buffer they point to is qdb_alloc'd here and freed by the destroy
  ;; functions below.

  ;; ---- arena of rows ----

  (fun ra_init ((var arena <ref RowArena>) (var row_bytes i64) (var block_rows i64))
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
  (fun ra_new_row ((var arena <ref RowArena>)) -> <ptr u8>
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

  (fun ra_destroy ((var arena <ref RowArena>))
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

  ;; ---- scan of the arena, in insertion order ----
  ;; (var b = 0)
  ;; (var end = 0)
  ;; (var row = (call ra_first_row arena b end))
  ;; (while (!= (cast row i64) 0)
  ;;   (block ... (= row (call ra_next_row arena b end row))))
  ;; The cursor is the index of the block that holds `row` and the address
  ;; past its last row, so that a step inside a block is one compare.

  ;; The address past the rows of block b: a block holds BlockRows rows, the
  ;; last one CurRows.
  (fun ra_block_end ((var arena <ref RowArena>) (var b i64)) -> i64
    (attrs inline)
    (block
      (var rows = (field arena BlockRows))
      (if (== b (- (field arena BlockCount) 1)) (block (= rows (field arena CurRows))))
      (var blocks = (field arena Blocks))
      (return (+ (cast (index blocks b) i64) (* rows (field arena RowBytes))))))

  (fun ra_first_row ((var arena <ref RowArena>) (var block <ref i64>)
                     (var end <ref i64>)) -> <ptr u8>
    (attrs inline)
    (block
      (= block 0)
      (= end 0)
      (if (== (field arena BlockCount) 0) (block (return (cast 0 <ptr u8>))))
      (= end (call ra_block_end arena 0))
      (var blocks = (field arena Blocks))
      (return (index blocks 0))))

  ;; The row after `row`; null after the last.
  (fun ra_next_row ((var arena <ref RowArena>) (var block <ref i64>)
                    (var end <ref i64>) (var row <ptr u8>)) -> <ptr u8>
    (attrs inline)
    (block
      (var next = (+ (cast row i64) (field arena RowBytes)))
      (if (< next end) (block (return (cast next <ptr u8>))))
      (var b = (+ block 1))
      (= block b)
      (if (== b (field arena BlockCount)) (block (return (cast 0 <ptr u8>))))
      (= end (call ra_block_end arena b))
      (var blocks = (field arena Blocks))
      (return (index blocks b))))

  (fun rt_row_hash ((var row <ptr u8>)) -> u64
    (block
      (var words = (cast row <ptr u64>))
      (return (index words 0))))

  ;; ---- entry table ----

  (fun rt_init ((var table <ref RowTable>) (var rows i64)) -> bool
    (block
      ;; Load factor at most 1/2, where linear probing reads about 1.5 entries
      ;; for a hit and 2.5 for a miss (Knuth, TAOCP vol. 3, section 6.4); the
      ;; minimum keeps the shift well defined and the table trivially small.
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
      (field_assign table Keys 0)
      (return #t)))

  (fun rt_destroy ((var table <ref RowTable>))
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
  (fun rt_mix ((var hash u64)) -> u64
    (attrs inline)
    (block (return (* hash (: -7046029254386353131 u64)))))

  (fun rt_salt ((var hash u64)) -> u64
    (attrs inline)
    (block (return (& (>> hash (: 16 u64)) (: 65535 u64)))))

  (fun rt_entry ((var salt u64) (var row <ptr u8>)) -> u64
    (attrs inline)
    (block (return (| (<< salt (: 48 u64)) (cast (cast row i64) u64)))))

  (fun rt_entry_row ((var entry u64)) -> <ptr u8>
    (attrs inline)
    (block (return (cast (cast (& entry (: 281474976710655 u64)) i64) <ptr u8>))))

  ;; Walks from `off` to the first entry that is empty or carries `salt`.
  ;; Returns that entry (0 when empty) and leaves its offset in out_off.
  (fun rt_seek ((var table <ref RowTable>) (var off u64) (var salt u64)
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

  ;; ---- lookup scratch: one slot per looked-up row ----

  (fun rt_scratch_destroy ((var s <ref RowScratch>))
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

  ;; key_bytes is the size of the caller's key struct.
  (fun rt_scratch_reserve ((var s <ref RowScratch>) (var n i64) (var key_bytes i64)) -> bool
    (block
      (if (<= n (field s Capacity)) (block (return #t)))
      (call rt_scratch_destroy s)
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

  ;; Pointer pass over Hashes[0, count): the entry for every looked-up row,
  ;; its salt and the offset it was found at. The rows are independent, so the
  ;; loop overlaps their cache misses.
  (fun rt_seek_all ((var table <ref RowTable>) (var s <ref RowScratch>) (var count i64))
    (block
      (var hashes = (field s Hashes))
      (var salts = (field s Salts))
      (var offsets = (field s Offsets))
      (var found = (field s Entries))
      (var shift = (cast (field table Shift) u64))
      (var j = 0)
      (while (< j count)
        (block
          (var h = (call rt_mix (index hashes j)))
          (var salt = (call rt_salt h))
          (var off = (: 0 u64))
          (var e = (call rt_seek table (>> h shift) salt off))
          (= salts [j] salt)
          (= offsets [j] off)
          (= found [j] e)
          (= j (+ j 1))))))

  ;; Same for the rows in Work[0, count) whose keys did not match the entry
  ;; they were at; their Offsets were already advanced past it.
  (fun rt_seek_work ((var table <ref RowTable>) (var s <ref RowScratch>) (var count i64))
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
          (var e = (call rt_seek table (index offsets j) (index salts j) off))
          (= offsets [j] off)
          (= found [j] e)
          (= q (+ q 1))))))
)
