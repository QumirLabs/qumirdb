(block
  ;; Parquet column-chunk decoder. The host reads the bytes of one column
  ;; chunk (its pages, from the first page header to the end of the chunk)
  ;; and describes how its values map to a TColumn; this library walks the
  ;; pages and writes the values of consecutive rows into TColumn buffers.
  ;; The host supplies one primitive, qdb_parquet_decompress, so the same
  ;; code serves native execution and the browser.
  ;;
  ;; Entry points, at the end of this file:
  ;;   pq_begin_chunk  resets the walking state for a new column chunk;
  ;;   pq_decode       decodes the next `rows` values into an output column;
  ;;   pq_release      frees the buffers the decoder owns.
  ;; They return 0 or a negative error code:
  ;;   -1 corrupt data, -2 unsupported page or encoding, -3 decompression
  ;;   failed, -4 dictionary index out of range, -5 the chunk ended early,
  ;;   -6 allocation failed, -7 string data exceeds int32 offsets.
  ;;
  ;; Supported: flat columns (no repetition levels) with definition levels
  ;; 0..MaxDef; data pages v1 and v2; PLAIN, PLAIN_DICTIONARY and
  ;; RLE_DICTIONARY values, and RLE booleans. Every buffer a value or a run
  ;; is read from has at least 64 readable bytes after its end: copies and
  ;; bit unpacking load whole 8- or 16-byte words.

  ;; A byte range [Pos, End) of Base, with a sticky error flag.
  (type PqBytes <struct (Base <ptr u8>) (Pos i64) (End i64) (Error i64)>)

  ;; Parquet's RLE/bit-packed hybrid over [Pos, End) of Base. A literal run
  ;; reads bit-packed values from bit Bit; an RLE run repeats Value.
  (type PqRle <struct
    (Base <ptr u8>) (Pos i64) (End i64) (Width i64)
    (Left i64) (Literal i64) (Value i64) (Bit i64)>)

  ;; Selects the instance of pq_unpack_groups for a bit width.
  (type PqWidth [(const W i64)] <struct (Unused i64)>)

  ;; The PageHeader fields the decoder uses (parquet.thrift).
  (type PqPage <struct
    (Type i64) (UncompressedSize i64) (CompressedSize i64) (NumValues i64)
    (Encoding i64) (LevelEncoding i64) (DefLength i64) (RepLength i64)
    (Compressed i64)>)

  ;; One column chunk; mirrors TParquetColumnState in
  ;; qdb/io/parquet/decoder.cpp. The host sets the first block of fields.
  ;; Kind: 1 fixed-width values of Width bytes, copied verbatim;
  ;;       2 booleans, written as a bitmap;
  ;;       3 byte arrays, written as int32 offsets plus data.
  (type PqColumn <struct
    (Chunk <ptr u8>) (ChunkSize i64)
    (Codec i64) (Codecs <ptr i8>)
    (Kind i64) (Width i64) (MaxDef i64)
    ;; Walking state: the next page header, the values left in the current
    ;; data page and its value encoding.
    (Pos i64) (PageLeft i64) (Encoding i64)
    (Plain PqBytes) (PlainBit i64)
    (Levels PqRle) (Indices PqRle)
    ;; The dictionary of the chunk. For byte arrays, entry k of EntryBuf is
    ;; (offset << 32) | length of value k within Dict.
    (Dict <ptr u8>) (DictCount i64) (DictMaxLen i64)
    ;; Buffers owned by the decoder; capacities exclude the padding.
    (PageBuf <ptr u8>) (PageCap i64)
    (DictBuf <ptr u8>) (DictCap i64)
    (EntryBuf <ptr u64>) (EntryCap i64)
    (LevelBuf <ptr u32>) (LevelCap i64)
    (IndexBuf <ptr u32>) (IndexCap i64)>)

  ;; One output column of a batch; mirrors TParquetOutput. Rows [0, Row)
  ;; are written. Data holds Width bytes per row, a bitmap of booleans, or
  ;; DataSize bytes of strings in DataCap bytes of storage that pq_decode
  ;; grows with qdb_alloc. Offsets has a slot per row plus one; Mask, a
  ;; bit per row, is written only when MaxDef > 0.
  (type PqOutput <struct
    (Data <ptr u8>) (DataCap i64) (DataSize i64)
    (Offsets <ptr i32>) (Mask <ptr u8>) (NullCount i64) (Row i64)>)

  ;; ---- Memory ----

  (fun pq_load64 ((var base <ptr u8>) (var offset i64)) -> u64 (attrs inline)
    (block
      (var word = (cast (+ (cast base i64) offset) <ptr <vec u8 8>>))
      (return (bitcast (index word 0) u64))))

  (fun pq_load32 ((var base <ptr u8>) (var offset i64)) -> i64 (attrs inline)
    (block
      (return (cast (& (call pq_load64 base offset) (: 4294967295 u64)) i64))))

  ;; Copies size bytes in 16-byte pieces: both ranges need 15 spare bytes.
  (fun pq_copy_bytes ((var dst <ptr u8>) (var src <ptr u8>) (var size i64))
       (attrs inline)
    (block
      (var to = (cast dst <ptr <vec u8 16>>))
      (var from = (cast src <ptr <vec u8 16>>))
      (var k = 0)
      (while (< (* k 16) size)
        (block
          (= to [k] (index from k))
          (= k (+ k 1))))))

  (fun pq_at ((var base <ptr u8>) (var offset i64)) -> <ptr u8> (attrs inline)
    (block
      (return (cast (+ (cast base i64) offset) <ptr u8>))))

  (fun pq_free ((var buffer <ptr i8>))
    (block
      (if (!= (cast buffer i64) 0)
        (block (call qdb_free buffer)))))

  ;; A buffer of at least bytes plus padding; the old contents are dropped.
  (fun pq_buffer ((var old <ptr i8>) (var bytes i64)) -> <ptr i8>
    (block
      (call pq_free old)
      (return (call qdb_alloc (+ bytes 64)))))

  (fun pq_capacity ((var cap i64) (var need i64)) -> i64 (attrs inline)
    (block
      (var grown = (+ cap (// cap 2)))
      (if (> grown need) (block (return grown)))
      (return need)))

  (fun pq_reserve_page ((var col <ref PqColumn>) (var size i64)) -> <ptr u8>
    (block
      (if (> size (field col PageCap))
        (block
          (var cap = (call pq_capacity (field col PageCap) size))
          (var buffer = (call pq_buffer (cast (field col PageBuf) <ptr i8>) cap))
          (field_assign col PageBuf (cast buffer <ptr u8>))
          (field_assign col PageCap (if (== (cast buffer i64) 0) 0 cap))))
      (return (field col PageBuf))))

  (fun pq_reserve_dict ((var col <ref PqColumn>) (var size i64)) -> <ptr u8>
    (block
      (if (> size (field col DictCap))
        (block
          (var cap = (call pq_capacity (field col DictCap) size))
          (var buffer = (call pq_buffer (cast (field col DictBuf) <ptr i8>) cap))
          (field_assign col DictBuf (cast buffer <ptr u8>))
          (field_assign col DictCap (if (== (cast buffer i64) 0) 0 cap))))
      (return (field col DictBuf))))

  (fun pq_reserve_entries ((var col <ref PqColumn>) (var count i64)) -> <ptr u64>
    (block
      (if (> count (field col EntryCap))
        (block
          (var cap = (call pq_capacity (field col EntryCap) count))
          (var buffer = (call pq_buffer (cast (field col EntryBuf) <ptr i8>) (* cap 8)))
          (field_assign col EntryBuf (cast buffer <ptr u64>))
          (field_assign col EntryCap (if (== (cast buffer i64) 0) 0 cap))))
      (return (field col EntryBuf))))

  (fun pq_reserve_levels ((var col <ref PqColumn>) (var count i64)) -> <ptr u32>
    (block
      (if (> count (field col LevelCap))
        (block
          (var cap = (call pq_capacity (field col LevelCap) count))
          (var buffer = (call pq_buffer (cast (field col LevelBuf) <ptr i8>) (* cap 4)))
          (field_assign col LevelBuf (cast buffer <ptr u32>))
          (field_assign col LevelCap (if (== (cast buffer i64) 0) 0 cap))))
      (return (field col LevelBuf))))

  (fun pq_reserve_indices ((var col <ref PqColumn>) (var count i64)) -> <ptr u32>
    (block
      (if (> count (field col IndexCap))
        (block
          (var cap = (call pq_capacity (field col IndexCap) count))
          (var buffer = (call pq_buffer (cast (field col IndexBuf) <ptr i8>) (* cap 4)))
          (field_assign col IndexBuf (cast buffer <ptr u32>))
          (field_assign col IndexCap (if (== (cast buffer i64) 0) 0 cap))))
      (return (field col IndexBuf))))

  ;; Grows the string storage of out to need bytes, keeping DataSize bytes.
  ;; Only runtime symbols are called here, not qdb_realloc: a library
  ;; function that calls a non-cacheable oz function is itself compiled
  ;; into every query instead of the persistent JIT cache.
  (fun pq_reserve_output ((var out <ref PqOutput>) (var need i64)) -> i64
    (block
      (if (<= need (field out DataCap)) (block (return 0)))
      (var cap = (* (field out DataCap) 2))
      (if (< cap need) (block (= cap need)))
      (var data = (cast (call qdb_alloc (+ cap 64)) <ptr u8>))
      (if (== (cast data i64) 0) (block (return -6)))
      (var old = (field out Data))
      (if (!= (cast old i64) 0)
        (block
          (var copied = (call builtin::memcpy data old (field out DataSize)))
          (call qdb_free (cast old <ptr i8>))))
      (field_assign out Data data)
      (field_assign out DataCap cap)
      (return 0)))

  ;; ---- Thrift compact protocol ----

  (fun pq_read_byte ((var r <ref PqBytes>)) -> i64
    (block
      (var pos = (field r Pos))
      (if (>= pos (field r End))
        (block
          (field_assign r Error 1)
          (return 0)))
      (var base = (field r Base))
      (field_assign r Pos (+ pos 1))
      (return (cast (index base pos) i64))))

  (fun pq_varint ((var r <ref PqBytes>)) -> u64
    (block
      (var result = (: 0 u64))
      (var shift = (: 0 u64))
      (var more = #t)
      (while more
        (block
          (var b = (cast (call pq_read_byte r) u64))
          (= result (| result (<< (& b (: 127 u64)) shift)))
          (= shift (+ shift (: 7 u64)))
          (= more (&& (!= (& b (: 128 u64)) (: 0 u64)) (< shift (: 64 u64))))))
      (return result)))

  (fun pq_zigzag ((var v u64)) -> i64 (attrs inline)
    (block
      (return (bitcast (^ (>> v (: 1 u64)) (- (: 0 u64) (& v (: 1 u64)))) i64))))

  (fun pq_int ((var r <ref PqBytes>)) -> i64
    (block (return (call pq_zigzag (call pq_varint r)))))

  (fun pq_skip_bytes ((var r <ref PqBytes>) (var count i64))
    (block
      (var pos = (field r Pos))
      (if (|| (< count 0) (> count (- (field r End) pos)))
        (block
          (field_assign r Error 1)
          (return)))
      (field_assign r Pos (+ pos count))))

  ;; Reads a field header. Returns the field type, 0 at the end of a struct,
  ;; and advances id to the field id.
  (fun pq_field ((var r <ref PqBytes>) (var id <ref i64>)) -> i64
    (block
      (var header = (call pq_read_byte r))
      (if (== header 0) (block (return 0)))
      (var delta = (>> header 4))
      (if (== delta 0)
        (block (= id (call pq_int r)))
        (block (= id (+ id delta))))
      (return (& header 15))))

  ;; Skips a value of a compact type; booleans inside a struct carry their
  ;; value in the field type and have no payload.
  (fun pq_skip ((var r <ref PqBytes>) (var ftype i64) (var depth i64))
    (block
      (if (> depth 32)
        (block
          (field_assign r Error 1)
          (return)))
      (if (|| (== ftype 1) (== ftype 2)) (block (return)))
      (if (== ftype 3) (block (call pq_skip_bytes r 1) (return)))
      (if (|| (== ftype 4) (|| (== ftype 5) (== ftype 6)))
        (block
          (var skipped = (call pq_varint r))
          (return)))
      (if (== ftype 7) (block (call pq_skip_bytes r 8) (return)))
      (if (== ftype 8)
        (block
          (call pq_skip_bytes r (bitcast (call pq_varint r) i64))
          (return)))
      (if (|| (== ftype 9) (== ftype 10))
        (block
          (var header = (call pq_read_byte r))
          (var size = (>> header 4))
          (if (== size 15) (block (= size (bitcast (call pq_varint r) i64))))
          (call pq_skip_items r (& header 15) size depth)
          (return)))
      (if (== ftype 11)
        (block
          (var size = (bitcast (call pq_varint r) i64))
          (if (> size 0)
            (block
              (var types = (call pq_read_byte r))
              (var i = 0)
              (while (&& (< i size) (== (field r Error) 0))
                (block
                  (call pq_skip_items r (>> types 4) 1 depth)
                  (call pq_skip_items r (& types 15) 1 depth)
                  (= i (+ i 1))))))
          (return)))
      (if (== ftype 12) (block (call pq_skip_struct r (+ depth 1)) (return)))
      (field_assign r Error 1)))

  ;; Skips size elements of a list, set or map; boolean elements are bytes.
  (fun pq_skip_items ((var r <ref PqBytes>) (var ftype i64) (var size i64) (var depth i64))
    (block
      (var i = 0)
      (while (&& (< i size) (== (field r Error) 0))
        (block
          (if (|| (== ftype 1) (== ftype 2))
            (block (call pq_skip_bytes r 1))
            (block (call pq_skip r ftype (+ depth 1))))
          (= i (+ i 1))))))

  (fun pq_skip_struct ((var r <ref PqBytes>) (var depth i64))
    (block
      (var id = 0)
      (while (== (field r Error) 0)
        (block
          (var ftype = (call pq_field r id))
          (if (== ftype 0) (block (return)))
          (call pq_skip r ftype depth)))))

  ;; DataPageHeader and DictionaryPageHeader share the leading fields
  ;; num_values (1) and encoding (2); a data page adds the definition level
  ;; encoding (3).
  (fun pq_parse_data_header ((var r <ref PqBytes>) (var page <ref PqPage>))
    (block
      (var id = 0)
      (while (== (field r Error) 0)
        (block
          (var ftype = (call pq_field r id))
          (if (== ftype 0) (block (return)))
          (var used = #f)
          (if (== ftype 5)
            (block
              (if (== id 1) (block (field_assign page NumValues (call pq_int r)) (= used #t)))
              (if (== id 2) (block (field_assign page Encoding (call pq_int r)) (= used #t)))
              (if (== id 3) (block (field_assign page LevelEncoding (call pq_int r)) (= used #t)))))
          (if (! used) (block (call pq_skip r ftype 1)))))))

  (fun pq_parse_data_header_v2 ((var r <ref PqBytes>) (var page <ref PqPage>))
    (block
      (var id = 0)
      (while (== (field r Error) 0)
        (block
          (var ftype = (call pq_field r id))
          (if (== ftype 0) (block (return)))
          (var used = #f)
          (if (== ftype 5)
            (block
              (if (== id 1) (block (field_assign page NumValues (call pq_int r)) (= used #t)))
              (if (== id 4) (block (field_assign page Encoding (call pq_int r)) (= used #t)))
              (if (== id 5) (block (field_assign page DefLength (call pq_int r)) (= used #t)))
              (if (== id 6) (block (field_assign page RepLength (call pq_int r)) (= used #t)))))
          (if (&& (== id 7) (|| (== ftype 1) (== ftype 2)))
            (block
              (field_assign page Compressed (if (== ftype 1) 1 0))
              (= used #t)))
          (if (! used) (block (call pq_skip r ftype 1)))))))

  (fun pq_parse_page_header ((var r <ref PqBytes>) (var page <ref PqPage>))
    (block
      (var id = 0)
      (while (== (field r Error) 0)
        (block
          (var ftype = (call pq_field r id))
          (if (== ftype 0) (block (return)))
          (var used = #f)
          (if (== ftype 5)
            (block
              (if (== id 1) (block (field_assign page Type (call pq_int r)) (= used #t)))
              (if (== id 2) (block (field_assign page UncompressedSize (call pq_int r)) (= used #t)))
              (if (== id 3) (block (field_assign page CompressedSize (call pq_int r)) (= used #t)))))
          (if (== ftype 12)
            (block
              (if (|| (== id 5) (== id 7))
                (block (call pq_parse_data_header r page) (= used #t)))
              (if (== id 8)
                (block (call pq_parse_data_header_v2 r page) (= used #t)))))
          (if (! used) (block (call pq_skip r ftype 0)))))))

  ;; ---- RLE/bit-packed hybrid ----

  (fun pq_bit_width ((var max i64)) -> i64
    (block
      (var width = 0)
      (while (> max 0)
        (block
          (= width (+ width 1))
          (= max (>> max 1))))
      (return width)))

  (fun pq_rle_init ((var rle <ref PqRle>) (var base <ptr u8>) (var pos i64)
                    (var end i64) (var width i64))
    (block
      (field_assign rle Base base)
      (field_assign rle Pos pos)
      (field_assign rle End end)
      (field_assign rle Width width)
      (field_assign rle Left 0)
      (field_assign rle Literal 0)
      (field_assign rle Value 0)
      (field_assign rle Bit 0)))

  ;; Reads the next run header. Returns 0 when the data is exhausted or
  ;; corrupt. A literal run truncated by the end of the data keeps the values
  ;; that are present.
  (fun pq_rle_next_run ((var rle <ref PqRle>)) -> i64
    (block
      (var base = (field rle Base))
      (var pos = (field rle Pos))
      (var end = (field rle End))
      (var header = (: 0 u64))
      (var shift = (: 0 u64))
      (var more = #t)
      (while more
        (block
          (if (>= pos end) (block (return 0)))
          (var b = (cast (index base pos) u64))
          (= pos (+ pos 1))
          (= header (| header (<< (& b (: 127 u64)) shift)))
          (= shift (+ shift (: 7 u64)))
          (= more (&& (!= (& b (: 128 u64)) (: 0 u64)) (< shift (: 64 u64))))))
      (var width = (field rle Width))
      (var count = (bitcast (>> header (: 1 u64)) i64))
      (if (!= (& header (: 1 u64)) (: 0 u64))
        (block
          ;; count groups of eight values, width bytes each
          (if (> count (: 1099511627776 i64)) (block (= count (: 1099511627776 i64))))
          (var bytes = (* count width))
          (= count (* count 8))
          (if (> bytes (- end pos))
            (block
              (= bytes (- end pos))
              (= count (// (* bytes 8) width))))
          (field_assign rle Literal 1)
          (field_assign rle Bit (* pos 8))
          (field_assign rle Pos (+ pos bytes)))
        (block
          (var bytes = (// (+ width 7) 8))
          (if (> bytes (- end pos)) (block (return 0)))
          (var value = 0)
          (var k = 0)
          (while (< k bytes)
            (block
              (= value (| value (<< (cast (index base (+ pos k)) i64) (* k 8))))
              (= k (+ k 1))))
          (field_assign rle Literal 0)
          (field_assign rle Value value)
          (field_assign rle Pos (+ pos bytes))))
      (field_assign rle Left count)
      (return 1)))

  ;; Unpacks groups of eight W-bit values from a byte-aligned src. W is a
  ;; compile-time constant, so every byte offset and shift folds.
  (fun pq_unpack_groups [(const W i64)]
       ((var tag <named PqWidth [W]>) (var src <ptr u8>) (var groups i64) (var out <ptr u32>))
    (block
      (var mask = (- (<< (: 1 u64) (cast W u64)) (: 1 u64)))
      (var g = 0)
      (while (< g groups)
        (block
          (var p = (call pq_at src (* g W)))
          (var o = (* g 8))
          (= out [o] (cast (& (call pq_load64 p 0) mask) u32))
          (= out [(+ o 1)] (cast (& (>> (call pq_load64 p (>> (* 1 W) 3)) (cast (& (* 1 W) 7) u64)) mask) u32))
          (= out [(+ o 2)] (cast (& (>> (call pq_load64 p (>> (* 2 W) 3)) (cast (& (* 2 W) 7) u64)) mask) u32))
          (= out [(+ o 3)] (cast (& (>> (call pq_load64 p (>> (* 3 W) 3)) (cast (& (* 3 W) 7) u64)) mask) u32))
          (= out [(+ o 4)] (cast (& (>> (call pq_load64 p (>> (* 4 W) 3)) (cast (& (* 4 W) 7) u64)) mask) u32))
          (= out [(+ o 5)] (cast (& (>> (call pq_load64 p (>> (* 5 W) 3)) (cast (& (* 5 W) 7) u64)) mask) u32))
          (= out [(+ o 6)] (cast (& (>> (call pq_load64 p (>> (* 6 W) 3)) (cast (& (* 6 W) 7) u64)) mask) u32))
          (= out [(+ o 7)] (cast (& (>> (call pq_load64 p (>> (* 7 W) 3)) (cast (& (* 7 W) 7) u64)) mask) u32))
          (= g (+ g 1))))))

  (fun pq_unpack ((var width i64) (var src <ptr u8>) (var groups i64) (var out <ptr u32>))
    (block
      (if (== width 0)
        (block
          (var i = 0)
          (while (< i (* groups 8))
            (block
              (= out [i] (: 0 u32))
              (= i (+ i 1))))
          (return)))
      (if (== width 1) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [1]>) src groups out) (return)))
      (if (== width 2) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [2]>) src groups out) (return)))
      (if (== width 3) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [3]>) src groups out) (return)))
      (if (== width 4) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [4]>) src groups out) (return)))
      (if (== width 5) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [5]>) src groups out) (return)))
      (if (== width 6) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [6]>) src groups out) (return)))
      (if (== width 7) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [7]>) src groups out) (return)))
      (if (== width 8) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [8]>) src groups out) (return)))
      (if (== width 9) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [9]>) src groups out) (return)))
      (if (== width 10) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [10]>) src groups out) (return)))
      (if (== width 11) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [11]>) src groups out) (return)))
      (if (== width 12) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [12]>) src groups out) (return)))
      (if (== width 13) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [13]>) src groups out) (return)))
      (if (== width 14) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [14]>) src groups out) (return)))
      (if (== width 15) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [15]>) src groups out) (return)))
      (if (== width 16) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [16]>) src groups out) (return)))
      (if (== width 17) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [17]>) src groups out) (return)))
      (if (== width 18) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [18]>) src groups out) (return)))
      (if (== width 19) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [19]>) src groups out) (return)))
      (if (== width 20) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [20]>) src groups out) (return)))
      (if (== width 21) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [21]>) src groups out) (return)))
      (if (== width 22) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [22]>) src groups out) (return)))
      (if (== width 23) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [23]>) src groups out) (return)))
      (if (== width 24) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [24]>) src groups out) (return)))
      (if (== width 25) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [25]>) src groups out) (return)))
      (if (== width 26) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [26]>) src groups out) (return)))
      (if (== width 27) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [27]>) src groups out) (return)))
      (if (== width 28) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [28]>) src groups out) (return)))
      (if (== width 29) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [29]>) src groups out) (return)))
      (if (== width 30) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [30]>) src groups out) (return)))
      (if (== width 31) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [31]>) src groups out) (return)))
      (if (== width 32) (block (call pq_unpack_groups (: (struct ((Unused 0))) <named PqWidth [32]>) src groups out) (return)))))

  ;; Decodes the next n values into out. Returns the number decoded, less
  ;; than n only when the data ends.
  (fun pq_rle_read ((var rle <ref PqRle>) (var out <ptr u32>) (var n i64)) -> i64
    (block
      (var base = (field rle Base))
      (var width = (field rle Width))
      (var mask = (- (<< (: 1 u64) (cast width u64)) (: 1 u64)))
      (var got = 0)
      (while (< got n)
        (block
          (if (== (field rle Left) 0)
            (block
              (if (== (call pq_rle_next_run rle) 0) (block (return got)))))
          (var take = (field rle Left))
          (if (> take (- n got)) (block (= take (- n got))))
          (if (!= (field rle Literal) 0)
            (block
              ;; single values up to a byte boundary, whole groups of eight
              ;; from there, then the tail
              (var bit = (field rle Bit))
              (var i = 0)
              (while (&& (< i take) (!= (& bit 7) 0))
                (block
                  (var word = (call pq_load64 base (>> bit 3)))
                  (= out [(+ got i)]
                    (cast (& (>> word (cast (& bit 7) u64)) mask) u32))
                  (= bit (+ bit width))
                  (= i (+ i 1))))
              (var groups = (>> (- take i) 3))
              (if (> groups 0)
                (block
                  (call pq_unpack width (call pq_at base (>> bit 3)) groups
                    (cast (call pq_at (cast out <ptr u8>) (* (+ got i) 4)) <ptr u32>))
                  (= i (+ i (* groups 8)))
                  (= bit (+ bit (* (* groups 8) width)))))
              (while (< i take)
                (block
                  (var word = (call pq_load64 base (>> bit 3)))
                  (= out [(+ got i)]
                    (cast (& (>> word (cast (& bit 7) u64)) mask) u32))
                  (= bit (+ bit width))
                  (= i (+ i 1))))
              (field_assign rle Bit bit))
            (block
              (var value = (cast (field rle Value) u32))
              (var i = 0)
              (while (< i take)
                (block
                  (= out [(+ got i)] value)
                  (= i (+ i 1))))))
          (field_assign rle Left (- (field rle Left) take))
          (= got (+ got take))))
      (return got)))

  ;; ---- Pages ----

  ;; Makes the page body [offset, offset + stored) of the chunk available as
  ;; size uncompressed bytes. Returns null on failure.
  (fun pq_inflate ((var col <ref PqColumn>) (var offset i64) (var stored i64)
                   (var size i64) (var compressed i64) (var dictionary i64)) -> <ptr u8>
    (block
      (var none = (cast 0 <ptr u8>))
      (var src = (call pq_at (field col Chunk) offset))
      (if (== size 0) (block (return src)))
      (if (|| (== (field col Codec) 0) (== compressed 0))
        (block
          (if (!= stored size) (block (return none)))
          (return src)))
      (var dst = none)
      (if (!= dictionary 0)
        (block (= dst (call pq_reserve_dict col size)))
        (block (= dst (call pq_reserve_page col size))))
      (if (== (cast dst i64) 0) (block (return none)))
      (var produced = (call qdb_parquet_decompress (field col Codecs) (field col Codec)
                        src stored dst size))
      (if (!= produced size) (block (return none)))
      (return dst)))

  (fun pq_load_dictionary ((var col <ref PqColumn>) (var page <ref PqPage>) (var body i64)) -> i64
    (block
      (var encoding = (field page Encoding))
      (if (&& (!= encoding 0) (!= encoding 2)) (block (return -2)))
      (var count = (field page NumValues))
      (var size = (field page UncompressedSize))
      (if (< count 0) (block (return -1)))
      (var bytes = (call pq_inflate col body (field page CompressedSize) size 1 1))
      (if (== (cast bytes i64) 0) (block (return -3)))
      (field_assign col Dict bytes)
      (field_assign col DictCount 0)
      (var kind = (field col Kind))
      (if (== kind 1)
        (block
          (if (> count (// size (field col Width))) (block (return -1)))
          (field_assign col DictCount count)
          (return 0)))
      (if (!= kind 3) (block (return -2)))
      (var entries = (call pq_reserve_entries col count))
      (if (== (cast entries i64) 0) (block (return -6)))
      (var pos = 0)
      (var longest = 0)
      (var k = 0)
      (while (< k count)
        (block
          (if (> (+ pos 4) size) (block (return -1)))
          (var length = (call pq_load32 bytes pos))
          (= pos (+ pos 4))
          (if (> length (- size pos)) (block (return -1)))
          (= entries [k] (| (<< (cast pos u64) (: 32 u64)) (cast length u64)))
          (if (> length longest) (block (= longest length)))
          (= pos (+ pos length))
          (= k (+ k 1))))
      (field_assign col DictCount count)
      (field_assign col DictMaxLen longest)
      (return 0)))

  (fun pq_bytes_init ((var b <ref PqBytes>) (var base <ptr u8>) (var end i64))
    (block
      (field_assign b Base base)
      (field_assign b Pos 0)
      (field_assign b End end)
      (field_assign b Error 0)))

  (fun pq_bytes_seek ((var b <ref PqBytes>) (var pos i64))
    (block (field_assign b Pos pos)))

  ;; Prepares the value decoder of a data page whose values occupy size
  ;; bytes at values.
  (fun pq_init_values ((var col <ref PqColumn>) (var encoding i64)
                       (var values <ptr u8>) (var size i64) (var count i64)) -> i64
    (block
      (field_assign col Encoding encoding)
      (if (== encoding 0)
        (block
          (call pq_bytes_init (field col Plain) values size)
          (field_assign col PlainBit 0)
          (field_assign col PageLeft count)
          (return 0)))
      (if (|| (== encoding 2) (== encoding 8))
        (block
          (if (== (field col Kind) 2) (block (return -2)))
          ;; one byte of index bit width; a page of nulls may have no values
          (var width = 0)
          (if (> size 0) (block (= width (cast (index values 0) i64))))
          (if (> width 32) (block (return -1)))
          (call pq_rle_init (field col Indices) values 1 size width)
          (field_assign col PageLeft count)
          (return 0)))
      (if (&& (== encoding 3) (== (field col Kind) 2))
        (block
          ;; RLE booleans: a 4-byte length, then runs of width 1
          (if (< size 4) (block (return -1)))
          (var length = (call pq_load32 values 0))
          (if (> length (- size 4)) (block (return -1)))
          (call pq_rle_init (field col Indices) values 4 (+ 4 length) 1)
          (field_assign col PageLeft count)
          (return 0)))
      (return -2)))

  (fun pq_load_data_page ((var col <ref PqColumn>) (var page <ref PqPage>) (var body i64)) -> i64
    (block
      (var count = (field page NumValues))
      (var stored = (field page CompressedSize))
      (var size = (field page UncompressedSize))
      (if (< count 0) (block (return -1)))
      (var max_def = (field col MaxDef))
      (var level_width = (call pq_bit_width max_def))
      (if (== (field page Type) 0)
        (block
          ;; v1: [definition levels: 4-byte length, runs][values], all
          ;; compressed together
          (var bytes = (call pq_inflate col body stored size 1 0))
          (if (== (cast bytes i64) 0) (block (return -3)))
          (var pos = 0)
          (if (> max_def 0)
            (block
              (if (!= (field page LevelEncoding) 3) (block (return -2)))
              (if (< size 4) (block (return -1)))
              (var length = (call pq_load32 bytes 0))
              (if (> length (- size 4)) (block (return -1)))
              (call pq_rle_init (field col Levels) bytes 4 (+ 4 length) level_width)
              (= pos (+ 4 length))))
          (return (call pq_init_values col (field page Encoding)
                    (call pq_at bytes pos) (- size pos) count))))
      ;; v2: [repetition levels][definition levels] uncompressed, then the
      ;; values, compressed unless the header says otherwise
      (var def_length = (field page DefLength))
      (if (!= (field page RepLength) 0) (block (return -2)))
      (if (|| (< def_length 0) (|| (> def_length stored) (> def_length size)))
        (block (return -1)))
      (if (&& (== max_def 0) (!= def_length 0)) (block (return -1)))
      (if (> max_def 0)
        (block
          (call pq_rle_init (field col Levels) (field col Chunk) body
            (+ body def_length) level_width)))
      (var value_size = (- size def_length))
      (var bytes = (call pq_inflate col (+ body def_length) (- stored def_length)
                     value_size (field page Compressed) 0))
      (if (== (cast bytes i64) 0) (block (return -3)))
      (return (call pq_init_values col (field page Encoding) bytes value_size count))))

  ;; Advances to the next data page with values, loading dictionary pages on
  ;; the way. This and pq_decode_rows are the out-of-line callees of the
  ;; entry point pq_decode: entry points are compiled with every query, and
  ;; noinline keeps that compilation from importing the decoder behind them.
  (fun pq_next_page ((var col <ref PqColumn>)) -> i64 (attrs noinline)
    (block
      (var size = (field col ChunkSize))
      (while (< (field col Pos) size)
        (block
          (var r = (: (struct ((Base (field col Chunk)) (Pos (field col Pos))
                               (End size) (Error 0))) PqBytes))
          (var page = (: (struct ((Type -1) (UncompressedSize -1) (CompressedSize -1)
                                  (NumValues -1) (Encoding -1) (LevelEncoding 3)
                                  (DefLength 0) (RepLength 0) (Compressed 1))) PqPage))
          (call pq_parse_page_header r page)
          (if (!= (field r Error) 0) (block (return -1)))
          (var body = (field r Pos))
          (var stored = (field page CompressedSize))
          (if (|| (< stored 0) (|| (> stored (- size body)) (< (field page UncompressedSize) 0)))
            (block (return -1)))
          (field_assign col Pos (+ body stored))
          (var ftype = (field page Type))
          (if (== ftype 2)
            (block
              (var status = (call pq_load_dictionary col page body))
              (if (< status 0) (block (return status)))))
          (if (|| (== ftype 0) (== ftype 3))
            (block
              (var status = (call pq_load_data_page col page body))
              (if (< status 0) (block (return status)))
              (if (> (field col PageLeft) 0) (block (return 0)))))))
      (return -5)))

  ;; ---- Values ----

  (fun pq_max_u32 ((var values <ptr u32>) (var n i64)) -> u32
    (block
      (var largest = (: 0 u32))
      (var i = 0)
      (while (< i n)
        (block
          (var value = (index values i))
          (if (> value largest) (block (= largest value)))
          (= i (+ i 1))))
      (return largest)))

  (fun pq_gather32 ((var dict <ptr u32>) (var indices <ptr u32>) (var out <ptr u32>) (var n i64))
    (block
      (var i = 0)
      (while (< i n)
        (block
          (= out [i] (index dict (cast (index indices i) i64)))
          (= i (+ i 1))))))

  (fun pq_gather64 ((var dict <ptr u64>) (var indices <ptr u32>) (var out <ptr u64>) (var n i64))
    (block
      (var i = 0)
      (while (< i n)
        (block
          (= out [i] (index dict (cast (index indices i) i64)))
          (= i (+ i 1))))))

  ;; Dictionary strings of at most 16 bytes: one 16-byte copy each.
  (fun pq_copy_short ((var dict <ptr u8>) (var entries <ptr u64>) (var indices <ptr u32>)
                      (var n i64) (var data <ptr u8>) (var pos i64)
                      (var offsets <ptr i32>) (var first i64)) -> i64
    (block
      (var base = (cast dict i64))
      (var target = (cast data i64))
      (var i = 0)
      (while (< i n)
        (block
          (var entry = (index entries (cast (index indices i) i64)))
          (var from = (cast (+ base (bitcast (>> entry (: 32 u64)) i64)) <ptr <vec u8 16>>))
          (var to = (cast (+ target pos) <ptr <vec u8 16>>))
          (= to [0] (index from 0))
          (= pos (+ pos (bitcast (& entry (: 4294967295 u64)) i64)))
          (= offsets [(+ first i)] (cast pos i32))
          (= i (+ i 1))))
      (return pos)))

  (fun pq_copy_long ((var dict <ptr u8>) (var entries <ptr u64>) (var indices <ptr u32>)
                     (var n i64) (var data <ptr u8>) (var pos i64)
                     (var offsets <ptr i32>) (var first i64)) -> i64
    (block
      (var i = 0)
      (while (< i n)
        (block
          (var entry = (index entries (cast (index indices i) i64)))
          (var length = (bitcast (& entry (: 4294967295 u64)) i64))
          (call pq_copy_bytes (call pq_at data pos)
            (call pq_at dict (bitcast (>> entry (: 32 u64)) i64)) length)
          (= pos (+ pos length))
          (= offsets [(+ first i)] (cast pos i32))
          (= i (+ i 1))))
      (return pos)))

  ;; Indices are decoded in blocks that stay in the L1 cache while their
  ;; values are looked up.
  (fun pq_decode_dictionary ((var col <ref PqColumn>) (var out <ref PqOutput>)
                             (var row i64) (var n i64)) -> i64
    (block
      (var block_rows = 1024)
      (var indices = (call pq_reserve_indices col block_rows))
      (if (== (cast indices i64) 0) (block (return -6)))
      (var kind = (field col Kind))
      (var width = (field col Width))
      (if (&& (!= kind 3) (|| (!= kind 1) (&& (!= width 4) (!= width 8))))
        (block (return -2)))
      (var dict = (field col Dict))
      (var count = (field col DictCount))
      (var longest = (field col DictMaxLen))
      (if (== kind 3)
        (block
          (if (< (call pq_reserve_output out (+ (field out DataSize) (* n longest) 16)) 0)
            (block (return -6)))))
      (var data = (field out Data))
      (var offsets = (field out Offsets))
      (var pos = (field out DataSize))
      (var done = 0)
      (while (< done n)
        (block
          (var m = (- n done))
          (if (> m block_rows) (block (= m block_rows)))
          (if (!= (call pq_rle_read (field col Indices) indices m) m) (block (return -1)))
          (if (>= (cast (call pq_max_u32 indices m) i64) count) (block (return -4)))
          (var at = (+ row done))
          (if (== kind 3)
            (block
              (if (<= longest 16)
                (block
                  (= pos (call pq_copy_short dict (field col EntryBuf) indices m data pos
                           offsets (+ at 1))))
                (block
                  (= pos (call pq_copy_long dict (field col EntryBuf) indices m data pos
                           offsets (+ at 1))))))
            (block
              (if (== width 4)
                (block
                  (call pq_gather32 (cast dict <ptr u32>) indices
                    (cast (call pq_at data (* at 4)) <ptr u32>) m))
                (block
                  (call pq_gather64 (cast dict <ptr u64>) indices
                    (cast (call pq_at data (* at 8)) <ptr u64>) m)))))
          (= done (+ done m))))
      (if (== kind 3)
        (block
          (if (> pos (: 2147483647 i64)) (block (return -7)))
          (field_assign out DataSize pos)))
      (return 0)))

  (fun pq_plain_fixed ((var plain <ref PqBytes>) (var data <ptr u8>) (var row i64)
                       (var n i64) (var width i64)) -> i64
    (block
      (var bytes = (* n width))
      (var pos = (field plain Pos))
      (if (> bytes (- (field plain End) pos)) (block (return -1)))
      (var copied = (call builtin::memcpy (call pq_at data (* row width))
                      (call pq_at (field plain Base) pos) bytes))
      (field_assign plain Pos (+ pos bytes))
      (return 0)))

  (fun pq_plain_strings ((var plain <ref PqBytes>) (var out <ref PqOutput>)
                         (var row i64) (var n i64)) -> i64
    (block
      (var base = (field plain Base))
      (var pos = (field plain Pos))
      (var end = (field plain End))
      (var data = (field out Data))
      (var size = (field out DataSize))
      (var cap = (field out DataCap))
      (var offsets = (field out Offsets))
      (var i = 0)
      (while (< i n)
        (block
          (if (> (+ pos 4) end) (block (return -1)))
          (var length = (call pq_load32 base pos))
          (= pos (+ pos 4))
          (if (> length (- end pos)) (block (return -1)))
          (if (> (+ size length 16) cap)
            (block
              (field_assign out DataSize size)
              (if (< (call pq_reserve_output out (+ size length 16)) 0) (block (return -6)))
              (= data (field out Data))
              (= cap (field out DataCap))))
          (call pq_copy_bytes (call pq_at data size) (call pq_at base pos) length)
          (= size (+ size length))
          (= pos (+ pos length))
          (= offsets [(+ row i 1)] (cast size i32))
          (= i (+ i 1))))
      (if (> size (: 2147483647 i64)) (block (return -7)))
      (field_assign out DataSize size)
      (field_assign plain Pos pos)
      (return 0)))

  (fun pq_set_bit ((var bitmap <ptr u8>) (var at i64) (var value i64)) (attrs inline)
    (block
      (var byte = (>> at 3))
      (var bit = (<< 1 (& at 7)))
      (var old = (cast (index bitmap byte) i64))
      (if (!= value 0)
        (block (= bitmap [byte] (cast (| old bit) u8)))
        (block (= bitmap [byte] (cast (& old (^ bit 255)) u8))))))

  (fun pq_get_bit ((var bitmap <ptr u8>) (var at i64)) -> i64 (attrs inline)
    (block
      (return (& (>> (cast (index bitmap (>> at 3)) i64) (& at 7)) 1))))

  (fun pq_decode_booleans ((var col <ref PqColumn>) (var out <ref PqOutput>)
                           (var row i64) (var n i64)) -> i64
    (block
      (var data = (field out Data))
      (if (== (field col Encoding) 3)
        (block
          (var values = (call pq_reserve_indices col n))
          (if (== (cast values i64) 0) (block (return -6)))
          (if (!= (call pq_rle_read (field col Indices) values n) n) (block (return -1)))
          (var i = 0)
          (while (< i n)
            (block
              (call pq_set_bit data (+ row i) (cast (index values i) i64))
              (= i (+ i 1))))
          (return 0)))
      ;; PLAIN: one bit per value, least significant bit first
      (var first = (+ (* (field (field col Plain) Pos) 8) (field col PlainBit)))
      (if (> (+ first n) (* (field (field col Plain) End) 8)) (block (return -1)))
      (var bits = (field (field col Plain) Base))
      (var i = 0)
      (while (< i n)
        (block
          (call pq_set_bit data (+ row i) (call pq_get_bit bits (+ first i)))
          (= i (+ i 1))))
      (var next = (+ first n))
      (call pq_bytes_seek (field col Plain) (>> next 3))
      (field_assign col PlainBit (& next 7))
      (return 0)))

  ;; Decodes n non-null values into rows [row, row + n).
  (fun pq_decode_values ((var col <ref PqColumn>) (var out <ref PqOutput>)
                         (var row i64) (var n i64)) -> i64
    (block
      (if (== n 0) (block (return 0)))
      (var kind = (field col Kind))
      (if (== kind 2) (block (return (call pq_decode_booleans col out row n))))
      (var encoding = (field col Encoding))
      (if (|| (== encoding 2) (== encoding 8))
        (block (return (call pq_decode_dictionary col out row n))))
      (if (!= encoding 0) (block (return -2)))
      (if (== kind 1)
        (block
          (return (call pq_plain_fixed (field col Plain) (field out Data) row n
                    (field col Width)))))
      (if (== kind 3)
        (block (return (call pq_plain_strings (field col Plain) out row n))))
      (return -2)))

  ;; Writes the validity bits of rows [row, row + n) and returns how many
  ;; are valid.
  (fun pq_write_mask ((var mask <ptr u8>) (var row i64) (var levels <ptr u32>)
                      (var n i64) (var max_def u32)) -> i64
    (block
      (var valid = 0)
      (var i = 0)
      (while (< i n)
        (block
          (var present = (if (== (index levels i) max_def) 1 0))
          (call pq_set_bit mask (+ row i) present)
          (= valid (+ valid present))
          (= i (+ i 1))))
      (return valid)))

  ;; The valid values of rows [row, row + n) were decoded densely into the
  ;; first `valid` slots; move each to its row, back to front, and give
  ;; null rows a zero value or an empty string.
  (fun pq_spread ((var col <ref PqColumn>) (var out <ref PqOutput>) (var row i64)
                  (var levels <ptr u32>) (var n i64) (var valid i64) (var max_def u32))
    (block
      (var kind = (field col Kind))
      (var j = (- valid 1))
      (var i = (- n 1))
      (if (&& (== kind 1) (== (field col Width) 4))
        (block
          (var data = (cast (call pq_at (field out Data) (* row 4)) <ptr u32>))
          (while (>= i 0)
            (block
              (if (== (index levels i) max_def)
                (block
                  (= data [i] (index data j))
                  (= j (- j 1)))
                (block (= data [i] (: 0 u32))))
              (= i (- i 1))))
          (return)))
      (if (&& (== kind 1) (== (field col Width) 8))
        (block
          (var data = (cast (call pq_at (field out Data) (* row 8)) <ptr u64>))
          (while (>= i 0)
            (block
              (if (== (index levels i) max_def)
                (block
                  (= data [i] (index data j))
                  (= j (- j 1)))
                (block (= data [i] (: 0 u64))))
              (= i (- i 1))))
          (return)))
      (if (== kind 2)
        (block
          (var data = (field out Data))
          (while (>= i 0)
            (block
              (if (== (index levels i) max_def)
                (block
                  (call pq_set_bit data (+ row i) (call pq_get_bit data (+ row j)))
                  (= j (- j 1)))
                (block (call pq_set_bit data (+ row i) 0)))
              (= i (- i 1))))
          (return)))
      (if (== kind 3)
        (block
          ;; offsets[row + 1 + k] ends value k; a null ends where the
          ;; previous value does
          (var offsets = (field out Offsets))
          (var base = (+ row 1))
          (while (>= i 0)
            (block
              (if (== (index levels i) max_def)
                (block
                  (= offsets [(+ base i)] (index offsets (+ base j)))
                  (= j (- j 1)))
                (block
                  (if (>= j 0)
                    (block (= offsets [(+ base i)] (index offsets (+ base j))))
                    (block (= offsets [(+ base i)] (index offsets row))))))
              (= i (- i 1))))))))

  (fun pq_decode_rows ((var col <ref PqColumn>) (var out <ref PqOutput>) (var n i64)) -> i64
       (attrs noinline)
    (block
      (var row = (field out Row))
      (if (== (field col MaxDef) 0)
        (block (return (call pq_decode_values col out row n))))
      (var levels = (call pq_reserve_levels col n))
      (if (== (cast levels i64) 0) (block (return -6)))
      (if (!= (call pq_rle_read (field col Levels) levels n) n) (block (return -1)))
      (var max_def = (cast (field col MaxDef) u32))
      (var valid = (call pq_write_mask (field out Mask) row levels n max_def))
      (field_assign out NullCount (+ (field out NullCount) (- n valid)))
      (var status = (call pq_decode_values col out row valid))
      (if (< status 0) (block (return status)))
      (if (< valid n)
        (block (call pq_spread col out row levels n valid max_def)))
      (return 0)))

  ;; ---- Entry points ----

  (fun pq_begin_chunk ((var col <ref PqColumn>)) -> i64
    (block
      (field_assign col Pos 0)
      (field_assign col PageLeft 0)
      (field_assign col Encoding -1)
      (field_assign col Dict (cast 0 <ptr u8>))
      (field_assign col DictCount 0)
      (field_assign col DictMaxLen 0)
      (return 0)))

  (fun pq_decode ((var col <ref PqColumn>) (var out <ref PqOutput>) (var rows i64)) -> i64
    (block
      (if (&& (== (field col Kind) 3) (== (field out Row) 0))
        (block
          (var offsets = (field out Offsets))
          (= offsets [0] (: 0 i32))))
      (var done = 0)
      (while (< done rows)
        (block
          (if (== (field col PageLeft) 0)
            (block
              (var status = (call pq_next_page col))
              (if (< status 0) (block (return status)))))
          (var take = (- rows done))
          (if (> take (field col PageLeft)) (block (= take (field col PageLeft))))
          (var status = (call pq_decode_rows col out take))
          (if (< status 0) (block (return status)))
          (field_assign col PageLeft (- (field col PageLeft) take))
          (field_assign out Row (+ (field out Row) take))
          (= done (+ done take))))
      (return 0)))

  (fun pq_release ((var col <ref PqColumn>)) -> i64
    (block
      (call pq_free (cast (field col PageBuf) <ptr i8>))
      (call pq_free (cast (field col DictBuf) <ptr i8>))
      (call pq_free (cast (field col EntryBuf) <ptr i8>))
      (call pq_free (cast (field col LevelBuf) <ptr i8>))
      (call pq_free (cast (field col IndexBuf) <ptr i8>))
      (field_assign col PageBuf (cast 0 <ptr u8>))
      (field_assign col PageCap 0)
      (field_assign col DictBuf (cast 0 <ptr u8>))
      (field_assign col DictCap 0)
      (field_assign col EntryBuf (cast 0 <ptr u64>))
      (field_assign col EntryCap 0)
      (field_assign col LevelBuf (cast 0 <ptr u32>))
      (field_assign col LevelCap 0)
      (field_assign col IndexBuf (cast 0 <ptr u32>))
      (field_assign col IndexCap 0)
      (return 0))))
