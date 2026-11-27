(block
  (pragma language overloads)

  ;; Row-id sort helpers. Row ids are packed as (batch << 32) | row, matching
  ;; the native TRowId helpers. The sort kernels below work directly over the
  ;; caller-owned row-id permutation and read key values from TRowSet columns.

  (fun sr_batch_index ((var row_id i64)) -> i64
    (block
      (return (>> row_id (: 32 i64)))))

  (fun sr_row_index ((var row_id i64)) -> i64
    (block
      (return (& row_id (: 4294967295 i64)))))

  (fun sr_column
       ((var store <ptr TRowSet>) (var row_id i64) (var column_idx i64)) -> TColumn
    (block
      (var batch = (index store (call sr_batch_index row_id)))
      (var columns = (field batch Columns))
      (return (index columns column_idx))))

  (fun sr_row_valid
       ((var store <ptr TRowSet>) (var row_id i64) (var column_idx i64)) -> bool
    (block
      (var col = (call sr_column store row_id column_idx))
      (if (== (cast (field col Mask) i64) (: 0 i64))
        (block (return #t)))
      (return (call bitoff
        (field col Mask)
        (call sr_row_index row_id)
        (cast (field col MaskBitOffset) i64)))))

  (fun sr_load_fixed_key [Value]
       ((var store <ptr TRowSet>)
        (var row_id i64)
        (var column_idx i64)
        (var type_witness <ptr Value>)) -> Value
    (block
      (var col = (call sr_column store row_id column_idx))
      (var data = (cast (cast (field col Data) i64) <ptr Value>))
      (return (index data (call sr_row_index row_id)))))

  (fun sr_load_bool_key
       ((var store <ptr TRowSet>) (var row_id i64) (var column_idx i64)) -> u8
    (block
      (var col = (call sr_column store row_id column_idx))
      (if (call bitoff
            (cast (cast (field col Data) i64) <ptr u8>)
            (call sr_row_index row_id)
            (cast (field col DataBitOffset) i64))
        (block (return (: 1 u8))))
      (return (: 0 u8))))

  ;; Fixed-width keys sort (key, row id) arrays: qumir_radix_key runs once per
  ;; row, and each pass reads the keys in order instead of fetching every value
  ;; through its row id. work holds keys and scattered keys of Key (u32 for keys
  ;; of at most 32 bits, else u64), then the scattered ids: RowIdSortOf's slots.

  ;; One stable counting pass over byte `digit` (bits) of keys, scattering keys
  ;; and ids into keys_out and ids_out. When every key has the same byte there,
  ;; it scatters nothing and returns #f.
  (fun sr_keyed_count_pass [Key]
       ((var keys <ptr Key>)
        (var ids <ptr i64>)
        (var keys_out <ptr Key>)
        (var ids_out <ptr i64>)
        (var counts <ptr u32>)
        (var n i64)
        (var digit i64)
        (var desc bool)) -> bool
    (block
      (var i = (: 0 i64))
      (while (< i (: 256 i64))
        (block
          (= counts [i] (: 0 u32))
          (= i (+ i (: 1 i64)))))
      (var shift = (cast digit Key))
      (var byte_mask = (cast (: 255 i64) Key))
      (= i (: 0 i64))
      (while (< i n)
        (block
          (var bucket = (cast (& (>> (index keys i) shift) byte_mask) i64))
          (= counts [bucket] (+ (index counts bucket) (: 1 u32)))
          (= i (+ i (: 1 i64)))))

      ;; Exclusive offsets in output order: DESC walks the buckets downwards.
      (var offset = (: 0 u32))
      (= i (: 0 i64))
      (while (< i (: 256 i64))
        (block
          (var bucket = i)
          (if desc
            (block (= bucket (- (: 255 i64) i))))
          (var bucket_rows = (index counts bucket))
          (if (== (cast bucket_rows i64) n)
            (block (return #f)))
          (= counts [bucket] offset)
          (= offset (+ offset bucket_rows))
          (= i (+ i (: 1 i64)))))

      (= i (: 0 i64))
      (while (< i n)
        (block
          (var key = (index keys i))
          (var bucket = (cast (& (>> key shift) byte_mask) i64))
          (var place = (cast (index counts bucket) i64))
          (= keys_out [place] key)
          (= ids_out [place] (index ids i))
          (= counts [bucket] (+ (index counts bucket) (: 1 u32)))
          (= i (+ i (: 1 i64)))))
      (return #t)))

  ;; Stable LSD sort of ids[0, n) by keys over key_bits bits. The passes move
  ;; the arrays back and forth; the sorted ids end in ids.
  (fun sr_keyed_sort [Key]
       ((var keys <ptr Key>)
        (var ids <ptr i64>)
        (var keys_work <ptr Key>)
        (var ids_work <ptr i64>)
        (var counts <ptr u32>)
        (var n i64)
        (var key_bits i64)
        (var desc bool))
    (block
      (var src_keys = keys)
      (var src_ids = ids)
      (var dst_keys = keys_work)
      (var dst_ids = ids_work)
      (var digit = (: 0 i64))
      (while (< digit key_bits)
        (block
          (if (call sr_keyed_count_pass src_keys src_ids dst_keys dst_ids counts n digit desc)
            (block
              (var keys_tmp = src_keys)
              (= src_keys dst_keys)
              (= dst_keys keys_tmp)
              (var ids_tmp = src_ids)
              (= src_ids dst_ids)
              (= dst_ids ids_tmp)))
          (= digit (+ digit (: 8 i64)))))
      (if (!= (cast src_ids i64) (cast ids i64))
        (block
          (var i = (: 0 i64))
          (while (< i n)
            (block
              (= ids [i] (index src_ids i))
              (= i (+ i (: 1 i64)))))))))

  (fun sort_fixed_rowids [Value Key]
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var row_ids <ptr i64>)
        (var work <ptr u64>)
        (var counts <ptr u32>)
        (var n i64)
        (var key_bits i64)
        (var desc bool)
        (var type_witness <ptr Value>)
        (var key_witness <ptr Key>)
        (var key_bytes i64))
    (block
      (var keys = (cast work <ptr Key>))
      (var keys_work = (cast (+ (cast work i64) (* n key_bytes)) <ptr Key>))
      (var ids_work = (cast (+ (cast work i64) (* n (* key_bytes (: 2 i64)))) <ptr i64>))
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (= keys [i] (cast (call qumir_radix_key
            (call sr_load_fixed_key store (index row_ids i) column_idx type_witness)) Key))
          (= i (+ i (: 1 i64)))))
      (call sr_keyed_sort keys row_ids keys_work ids_work counts n key_bits desc)))

  ;; NULL rows keep their input order at the chosen end, as in the 257-bucket
  ;; passes; only the valid rows are sorted.
  (fun sort_fixed_rowids_nullable [Value Key]
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var row_ids <ptr i64>)
        (var work <ptr u64>)
        (var counts <ptr u32>)
        (var n i64)
        (var key_bits i64)
        (var desc bool)
        (var nulls_first bool)
        (var type_witness <ptr Value>)
        (var key_witness <ptr Key>)
        (var key_bytes i64))
    (block
      (var keys = (cast work <ptr Key>))
      (var keys_work = (cast (+ (cast work i64) (* n key_bytes)) <ptr Key>))
      (var ids_work = (cast (+ (cast work i64) (* n (* key_bytes (: 2 i64)))) <ptr i64>))
      ;; Valid rows go to the front of ids_work, NULL rows to its back.
      (var valid = (: 0 i64))
      (var nulls = (: 0 i64))
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (var row = (index row_ids i))
          (if (call sr_row_valid store row column_idx)
            (block
              (= keys [valid] (cast (call qumir_radix_key
                (call sr_load_fixed_key store row column_idx type_witness)) Key))
              (= ids_work [valid] row)
              (= valid (+ valid (: 1 i64))))
            (block
              (= nulls (+ nulls (: 1 i64)))
              (= ids_work [(- n nulls)] row)))
          (= i (+ i (: 1 i64)))))

      (var valid_base = (: 0 i64))
      (var null_base = valid)
      (if nulls_first
        (block
          (= valid_base nulls)
          (= null_base (: 0 i64))))
      (= i (: 0 i64))
      (while (< i valid)
        (block
          (= row_ids [(+ valid_base i)] (index ids_work i))
          (= i (+ i (: 1 i64)))))
      (= i (: 0 i64))
      (while (< i nulls)
        (block
          (= row_ids [(+ null_base i)] (index ids_work (- (- n (: 1 i64)) i)))
          (= i (+ i (: 1 i64)))))
      (var valid_ids = (cast (+ (cast row_ids i64) (* valid_base (: 8 i64))) <ptr i64>))
      (call sr_keyed_sort keys valid_ids keys_work ids_work counts valid key_bits desc)))

  ;; BinInt (128-bit decimal) radix key for one 64-bit half. A signed 128-bit value
  ;; orders as (Hi signed, Lo unsigned): flip Hi's sign bit, leave Lo as-is. Radix is
  ;; stable + LSD, so sorting by Lo first and Hi last makes Hi the primary key.
  (fun sr_binint_radix_key
       ((var store <ptr TRowSet>)
        (var row_id i64)
        (var column_idx i64)
        (var use_hi bool)) -> u64
    (block
      (var witness = (cast (: 0 i64) <ptr BinInt>))
      (var v = (call sr_load_fixed_key store row_id column_idx witness))
      ;; BinInt is an i128: take its halves with a shift, and flip the sign bit
      ;; of the high half so radix ordering matches signed ordering.
      (var raw = (field v V))
      (if use_hi
        (block
          (return (^ (cast (>> raw (: 64 i128)) u64) (<< (: 1 u64) (: 63 u64))))))
      (return (cast raw u64))))

  (fun sort_binint_rowids_count_pass
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var row_ids <ptr i64>)
        (var work <ptr i64>)
        (var counts <ptr u32>)
        (var n i64)
        (var digit i64)
        (var desc bool)
        (var use_hi bool))
    (block
      (var i i64)
      (= i (: 0 i64))
      (while (< i (: 256 i64))
        (block
          (= counts [i] (: 0 u32))
          (= i (+ i (: 1 i64)))))

      (= i (: 0 i64))
      (while (< i n)
        (block
          (var row = (index row_ids i))
          (var bucket = (call qumir_radix_digit
            (call sr_binint_radix_key store row column_idx use_hi) digit))
          (if desc
            (block (= bucket (- (: 255 u32) bucket))))
          (var bucket_index = (cast bucket i64))
          (= counts [bucket_index] (+ (index counts bucket_index) (: 1 u32)))
          (= i (+ i (: 1 i64)))))

      (= i (: 1 i64))
      (while (< i (: 256 i64))
        (block
          (= counts [i] (+ (index counts i) (index counts (- i (: 1 i64)))))
          (= i (+ i (: 1 i64)))))

      (= i n)
      (while (> i (: 0 i64))
        (block
          (= i (- i (: 1 i64)))
          (var row = (index row_ids i))
          (var bucket = (call qumir_radix_digit
            (call sr_binint_radix_key store row column_idx use_hi) digit))
          (if desc
            (block (= bucket (- (: 255 u32) bucket))))
          (var bucket_index = (cast bucket i64))
          (var place = (- (index counts bucket_index) (: 1 u32)))
          (= work [(cast place i64)] row)
          (= counts [bucket_index] place)))

      (= i (: 0 i64))
      (while (< i n)
        (block
          (= row_ids [i] (index work i))
          (= i (+ i (: 1 i64)))))))

  (fun sort_binint_rowids
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var row_ids <ptr i64>)
        (var work <ptr i64>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (var digit i64)
      (= digit (: 0 i64))
      (while (< digit (: 64 i64))
        (block
          (call sort_binint_rowids_count_pass
            store column_idx row_ids work counts n digit desc #f)
          (= digit (+ digit (: 8 i64)))))
      (= digit (: 0 i64))
      (while (< digit (: 64 i64))
        (block
          (call sort_binint_rowids_count_pass
            store column_idx row_ids work counts n digit desc #t)
          (= digit (+ digit (: 8 i64)))))))

  (fun sort_binint_rowids_count_pass_nullable
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var row_ids <ptr i64>)
        (var work <ptr i64>)
        (var counts <ptr u32>)
        (var n i64)
        (var digit i64)
        (var desc bool)
        (var nulls_first bool)
        (var use_hi bool))
    (block
      (var i i64)
      (= i (: 0 i64))
      (while (< i (: 257 i64))
        (block
          (= counts [i] (: 0 u32))
          (= i (+ i (: 1 i64)))))

      (= i (: 0 i64))
      (while (< i n)
        (block
          (var row = (index row_ids i))
          (var bucket i64)
          (if (! (call sr_row_valid store row column_idx))
            (block
              (if nulls_first
                (block (= bucket (: 0 i64)))
                (block (= bucket (: 256 i64)))))
            (block
              (var digit_bucket = (cast (call qumir_radix_digit
                (call sr_binint_radix_key store row column_idx use_hi) digit) i64))
              (if desc
                (block (= digit_bucket (- (: 255 i64) digit_bucket))))
              (if nulls_first
                (block (= bucket (+ digit_bucket (: 1 i64))))
                (block (= bucket digit_bucket)))))
          (= counts [bucket] (+ (index counts bucket) (: 1 u32)))
          (= i (+ i (: 1 i64)))))

      (= i (: 1 i64))
      (while (< i (: 257 i64))
        (block
          (= counts [i] (+ (index counts i) (index counts (- i (: 1 i64)))))
          (= i (+ i (: 1 i64)))))

      (= i n)
      (while (> i (: 0 i64))
        (block
          (= i (- i (: 1 i64)))
          (var row = (index row_ids i))
          (var bucket i64)
          (if (! (call sr_row_valid store row column_idx))
            (block
              (if nulls_first
                (block (= bucket (: 0 i64)))
                (block (= bucket (: 256 i64)))))
            (block
              (var digit_bucket = (cast (call qumir_radix_digit
                (call sr_binint_radix_key store row column_idx use_hi) digit) i64))
              (if desc
                (block (= digit_bucket (- (: 255 i64) digit_bucket))))
              (if nulls_first
                (block (= bucket (+ digit_bucket (: 1 i64))))
                (block (= bucket digit_bucket)))))
          (var place = (- (index counts bucket) (: 1 u32)))
          (= work [(cast place i64)] row)
          (= counts [bucket] place)))

      (= i (: 0 i64))
      (while (< i n)
        (block
          (= row_ids [i] (index work i))
          (= i (+ i (: 1 i64)))))))

  (fun sort_binint_rowids_nullable
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var row_ids <ptr i64>)
        (var work <ptr i64>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool)
        (var nulls_first bool))
    (block
      (var digit i64)
      (= digit (: 0 i64))
      (while (< digit (: 64 i64))
        (block
          (call sort_binint_rowids_count_pass_nullable
            store column_idx row_ids work counts n digit desc nulls_first #f)
          (= digit (+ digit (: 8 i64)))))
      (= digit (: 0 i64))
      (while (< digit (: 64 i64))
        (block
          (call sort_binint_rowids_count_pass_nullable
            store column_idx row_ids work counts n digit desc nulls_first #t)
          (= digit (+ digit (: 8 i64)))))))

  (fun sort_bool_rowids_count_pass
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var row_ids <ptr i64>)
        (var work <ptr i64>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (var i i64)
      (= i (: 0 i64))
      (while (< i (: 256 i64))
        (block
          (= counts [i] (: 0 u32))
          (= i (+ i (: 1 i64)))))

      (= i (: 0 i64))
      (while (< i n)
        (block
          (var row = (index row_ids i))
          (var bucket = (cast (call sr_load_bool_key store row column_idx) u32))
          (if desc
            (block (= bucket (- (: 255 u32) bucket))))
          (var bucket_index = (cast bucket i64))
          (= counts [bucket_index] (+ (index counts bucket_index) (: 1 u32)))
          (= i (+ i (: 1 i64)))))

      (= i (: 1 i64))
      (while (< i (: 256 i64))
        (block
          (= counts [i] (+ (index counts i) (index counts (- i (: 1 i64)))))
          (= i (+ i (: 1 i64)))))

      (= i n)
      (while (> i (: 0 i64))
        (block
          (= i (- i (: 1 i64)))
          (var row = (index row_ids i))
          (var bucket = (cast (call sr_load_bool_key store row column_idx) u32))
          (if desc
            (block (= bucket (- (: 255 u32) bucket))))
          (var bucket_index = (cast bucket i64))
          (var place = (- (index counts bucket_index) (: 1 u32)))
          (= work [(cast place i64)] row)
          (= counts [bucket_index] place)))

      (= i (: 0 i64))
      (while (< i n)
        (block
          (= row_ids [i] (index work i))
          (= i (+ i (: 1 i64)))))))

  (fun sort_bool_rowids
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var row_ids <ptr i64>)
        (var work <ptr i64>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (call sort_bool_rowids_count_pass store column_idx row_ids work counts n desc)))

  (fun sort_bool_rowids_count_pass_nullable
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var row_ids <ptr i64>)
        (var work <ptr i64>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool)
        (var nulls_first bool))
    (block
      (var i i64)
      (= i (: 0 i64))
      (while (< i (: 257 i64))
        (block
          (= counts [i] (: 0 u32))
          (= i (+ i (: 1 i64)))))

      (= i (: 0 i64))
      (while (< i n)
        (block
          (var row = (index row_ids i))
          (var bucket i64)
          (if (! (call sr_row_valid store row column_idx))
            (block
              (if nulls_first
                (block (= bucket (: 0 i64)))
                (block (= bucket (: 256 i64)))))
            (block
              (var digit_bucket = (cast (call sr_load_bool_key store row column_idx) i64))
              (if desc
                (block (= digit_bucket (- (: 255 i64) digit_bucket))))
              (if nulls_first
                (block (= bucket (+ digit_bucket (: 1 i64))))
                (block (= bucket digit_bucket)))))
          (= counts [bucket] (+ (index counts bucket) (: 1 u32)))
          (= i (+ i (: 1 i64)))))

      (= i (: 1 i64))
      (while (< i (: 257 i64))
        (block
          (= counts [i] (+ (index counts i) (index counts (- i (: 1 i64)))))
          (= i (+ i (: 1 i64)))))

      (= i n)
      (while (> i (: 0 i64))
        (block
          (= i (- i (: 1 i64)))
          (var row = (index row_ids i))
          (var bucket i64)
          (if (! (call sr_row_valid store row column_idx))
            (block
              (if nulls_first
                (block (= bucket (: 0 i64)))
                (block (= bucket (: 256 i64)))))
            (block
              (var digit_bucket = (cast (call sr_load_bool_key store row column_idx) i64))
              (if desc
                (block (= digit_bucket (- (: 255 i64) digit_bucket))))
              (if nulls_first
                (block (= bucket (+ digit_bucket (: 1 i64))))
                (block (= bucket digit_bucket)))))
          (var place = (- (index counts bucket) (: 1 u32)))
          (= work [(cast place i64)] row)
          (= counts [bucket] place)))

      (= i (: 0 i64))
      (while (< i n)
        (block
          (= row_ids [i] (index work i))
          (= i (+ i (: 1 i64)))))))

  (fun sort_bool_rowids_nullable
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var row_ids <ptr i64>)
        (var work <ptr i64>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool)
        (var nulls_first bool))
    (block
      (call sort_bool_rowids_count_pass_nullable
        store column_idx row_ids work counts n desc nulls_first)))

  (fun sr_col_offset ((var col TColumn) (var i i64)) -> i64
    (block
      (if (== (cast (field col OffsetWidth) i64) (: 4 i64))
        (block
          (var offsets32 = (cast (cast (field col Offsets) i64) <ptr i32>))
          (return (cast (index offsets32 i) i64))))
      (var offsets64 = (cast (cast (field col Offsets) i64) <ptr i64>))
      (return (index offsets64 i))))

  (fun sr_col_begin ((var col TColumn) (var row i64)) -> i64
    (block
      (return (- (call sr_col_offset col row)
                 (call sr_col_offset col (: 0 i64))))))

  (fun sr_col_size ((var col TColumn) (var row i64)) -> i64
    (block
      (return (- (call sr_col_offset col (+ row (: 1 i64)))
                 (call sr_col_offset col row)))))

  ;; First 8 bytes as a big-endian u64; short strings are zero-padded.
  (fun sr_make_prefix
       ((var store <ptr TRowSet>) (var column_idx i64) (var row_id i64)) -> u64
    (block
      (var col = (call sr_column store row_id column_idx))
      (var data = (cast (cast (field col Data) i64) <ptr u8>))
      (var row = (call sr_row_index row_id))
      (var begin = (call sr_col_begin col row))
      (var size = (call sr_col_size col row))
      (var limit = size)
      (if (> limit (: 8 i64))
        (block (= limit (: 8 i64))))
      (var prefix = (: 0 u64))
      (var i i64)
      (= i (: 0 i64))
      (while (< i limit)
        (block
          (= prefix (| (<< prefix (: 8 u64))
                       (cast (index data (+ begin i)) u64)))
          (= i (+ i (: 1 i64)))))
      (= prefix (<< prefix (cast (* (: 8 i64) (- (: 8 i64) limit)) u64)))
      (return prefix)))

  ;; memcmp + length tie-break over row ids: -1 / 0 / 1.
  (fun sr_sort_compare
       ((var store <ptr TRowSet>) (var column_idx i64) (var a i64) (var b i64)) -> i64
    (block
      (var a_col = (call sr_column store a column_idx))
      (var b_col = (call sr_column store b column_idx))
      (var a_data = (cast (cast (field a_col Data) i64) <ptr u8>))
      (var b_data = (cast (cast (field b_col Data) i64) <ptr u8>))
      (var a_row = (call sr_row_index a))
      (var b_row = (call sr_row_index b))
      (var a_begin = (call sr_col_begin a_col a_row))
      (var b_begin = (call sr_col_begin b_col b_row))
      (var a_size = (call sr_col_size a_col a_row))
      (var b_size = (call sr_col_size b_col b_row))
      (var limit = a_size)
      (if (< b_size limit)
        (block (= limit b_size)))
      (var i i64)
      (= i (: 0 i64))
      (while (< i limit)
        (block
          (var a_byte = (cast (index a_data (+ a_begin i)) i64))
          (var b_byte = (cast (index b_data (+ b_begin i)) i64))
          (if (< a_byte b_byte)
            (block (return (: -1 i64))))
          (if (> a_byte b_byte)
            (block (return (: 1 i64))))
          (= i (+ i (: 1 i64)))))
      (if (< a_size b_size)
        (block (return (: -1 i64))))
      (if (> a_size b_size)
        (block (return (: 1 i64))))
      (return (: 0 i64))))

  ;; True when the pair at `a` must stay before/at the pair at `b`
  ;; (stable keep-order test over full strings).
  (fun sr_pair_keep_order
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var pairs <ptr u64>)
        (var a i64)
        (var b i64)
        (var desc bool)) -> bool
    (block
      (var row_a = (cast (index pairs (+ (* a (: 2 i64)) (: 1 i64))) i64))
      (var row_b = (cast (index pairs (+ (* b (: 2 i64)) (: 1 i64))) i64))
      (var cmp = (call sr_sort_compare store column_idx row_a row_b))
      (if desc
        (block (return (>= cmp (: 0 i64)))))
      (return (<= cmp (: 0 i64)))))

  ;; One stable counting pass over prefix byte `digit` (bits), from pairs into
  ;; work.
  (fun sr_pair_count_pass
       ((var pairs <ptr u64>)
        (var work <ptr u64>)
        (var counts <ptr u32>)
        (var n i64)
        (var digit i64)
        (var desc bool))
    (block
      (var i i64)
      (= i (: 0 i64))
      (while (< i (: 256 i64))
        (block
          (= counts [i] (: 0 u32))
          (= i (+ i (: 1 i64)))))

      (= i (: 0 i64))
      (while (< i n)
        (block
          (var bucket = (cast (& (>> (index pairs (* i (: 2 i64)))
                                     (cast digit u64))
                                 (: 255 u64)) i64))
          (if desc
            (block (= bucket (- (: 255 i64) bucket))))
          (= counts [bucket] (+ (index counts bucket) (: 1 u32)))
          (= i (+ i (: 1 i64)))))

      (= i (: 1 i64))
      (while (< i (: 256 i64))
        (block
          (= counts [i] (+ (index counts i) (index counts (- i (: 1 i64)))))
          (= i (+ i (: 1 i64)))))

      (= i n)
      (while (> i (: 0 i64))
        (block
          (= i (- i (: 1 i64)))
          (var bucket = (cast (& (>> (index pairs (* i (: 2 i64)))
                                     (cast digit u64))
                                 (: 255 u64)) i64))
          (if desc
            (block (= bucket (- (: 255 i64) bucket))))
          (var place = (cast (- (index counts bucket) (: 1 u32)) i64))
          (= work [(* place (: 2 i64))] (index pairs (* i (: 2 i64))))
          (= work [(+ (* place (: 2 i64)) (: 1 i64))]
             (index pairs (+ (* i (: 2 i64)) (: 1 i64))))
          (= counts [bucket] (cast place u32))))))

  ;; True when the row at pair position `j` must move past `key_row`.
  (fun sr_pair_shifts
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var pairs <ptr u64>)
        (var j i64)
        (var key_row i64)
        (var desc bool)) -> bool
    (block
      (var j_row = (cast (index pairs (+ (* j (: 2 i64)) (: 1 i64))) i64))
      (var cmp = (call sr_sort_compare store column_idx j_row key_row))
      (if desc
        (block (return (< cmp (: 0 i64)))))
      (return (> cmp (: 0 i64)))))

  ;; Stable insertion sort of pairs[lo, hi) by full string compare.
  (fun sr_pair_insertion
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var pairs <ptr u64>)
        (var lo i64)
        (var hi i64)
        (var desc bool))
    (block
      (var i = (+ lo (: 1 i64)))
      (while (< i hi)
        (block
          (var key_prefix = (index pairs (* i (: 2 i64))))
          (var key_row = (index pairs (+ (* i (: 2 i64)) (: 1 i64))))
          (var j = (- i (: 1 i64)))
          (var moving = #t)
          (while (&& (>= j lo) moving)
            (block
              (if (call sr_pair_shifts store column_idx
                    pairs j (cast key_row i64) desc)
                (block
                  (= pairs [(* (+ j (: 1 i64)) (: 2 i64))]
                     (index pairs (* j (: 2 i64))))
                  (= pairs [(+ (* (+ j (: 1 i64)) (: 2 i64)) (: 1 i64))]
                     (index pairs (+ (* j (: 2 i64)) (: 1 i64))))
                  (= j (- j (: 1 i64))))
                (block
                  (= moving #f)))))
          (= pairs [(* (+ j (: 1 i64)) (: 2 i64))] key_prefix)
          (= pairs [(+ (* (+ j (: 1 i64)) (: 2 i64)) (: 1 i64))] key_row)
          (= i (+ i (: 1 i64)))))))

  ;; Stable bottom-up merge sort of pairs[lo, hi); `work` holds pairs too.
  (fun sr_pair_merge_range
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var pairs <ptr u64>)
        (var work <ptr u64>)
        (var lo i64)
        (var hi i64)
        (var desc bool))
    (block
      (if (<= (- hi lo) (: 32 i64))
        (block
          (call sr_pair_insertion store column_idx pairs lo hi desc)
          (return)))

      (var block_lo = lo)
      (while (< block_lo hi)
        (block
          (var block_hi = (+ block_lo (: 32 i64)))
          (if (> block_hi hi)
            (block (= block_hi hi)))
          (call sr_pair_insertion store column_idx pairs block_lo block_hi desc)
          (= block_lo block_hi)))

      (var width = (: 32 i64))
      (while (< width (- hi lo))
        (block
          (var left = lo)
          (while (< left hi)
            (block
              (var mid = (+ left width))
              (var right = (+ mid width))
              (if (> mid hi)
                (block (= mid hi)))
              (if (> right hi)
                (block (= right hi)))
              (var i = left)
              (var j = mid)
              (var k = left)
              (while (&& (< i mid) (< j right))
                (block
                  (if (call sr_pair_keep_order store column_idx pairs i j desc)
                    (block
                      (= work [(* k (: 2 i64))] (index pairs (* i (: 2 i64))))
                      (= work [(+ (* k (: 2 i64)) (: 1 i64))]
                         (index pairs (+ (* i (: 2 i64)) (: 1 i64))))
                      (= i (+ i (: 1 i64))))
                    (block
                      (= work [(* k (: 2 i64))] (index pairs (* j (: 2 i64))))
                      (= work [(+ (* k (: 2 i64)) (: 1 i64))]
                         (index pairs (+ (* j (: 2 i64)) (: 1 i64))))
                      (= j (+ j (: 1 i64)))))
                  (= k (+ k (: 1 i64)))))
              (while (< i mid)
                (block
                  (= work [(* k (: 2 i64))] (index pairs (* i (: 2 i64))))
                  (= work [(+ (* k (: 2 i64)) (: 1 i64))]
                     (index pairs (+ (* i (: 2 i64)) (: 1 i64))))
                  (= i (+ i (: 1 i64)))
                  (= k (+ k (: 1 i64)))))
              (while (< j right)
                (block
                  (= work [(* k (: 2 i64))] (index pairs (* j (: 2 i64))))
                  (= work [(+ (* k (: 2 i64)) (: 1 i64))]
                     (index pairs (+ (* j (: 2 i64)) (: 1 i64))))
                  (= j (+ j (: 1 i64)))
                  (= k (+ k (: 1 i64)))))
              (var copy = (* left (: 2 i64)))
              (while (< copy (* right (: 2 i64)))
                (block
                  (= pairs [copy] (index work copy))
                  (= copy (+ copy (: 1 i64)))))
              (= left (+ left (* width (: 2 i64))))))
          (= width (* width (: 2 i64)))))))

  ;; Tie-break every run of equal prefixes in pairs[lo, hi).
  (fun sr_pair_tiebreak_runs
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var pairs <ptr u64>)
        (var work <ptr u64>)
        (var lo i64)
        (var hi i64)
        (var desc bool))
    (block
      (var run_start = lo)
      (var i = (+ lo (: 1 i64)))
      (while (<= i hi)
        (block
          (var boundary = (== i hi))
          (if (! boundary)
            (block
              (= boundary (!= (index pairs (* run_start (: 2 i64)))
                              (index pairs (* i (: 2 i64)))))))
          (if boundary
            (block
              (if (> (- i run_start) (: 1 i64))
                (block
                  (call sr_pair_merge_range store column_idx
                    pairs work run_start i desc)))
              (= run_start i)))
          (= i (+ i (: 1 i64)))))))

  (fun sr_pairs_to_row_ids ((var pairs <ptr u64>) (var row_ids <ptr i64>) (var n i64))
    (block
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (= row_ids [i] (cast (index pairs (+ (* i (: 2 i64)) (: 1 i64))) i64))
          (= i (+ i (: 1 i64)))))))

  ;; Entry: stable sort of row_ids[0, n) by one string column. `work` must hold
  ;; two pair arrays: 32 * n bytes.
  (fun sort_string_rowids
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var row_ids <ptr i64>)
        (var work <ptr u64>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (var pairs = work)
      (var pair_work = (cast (+ (cast pairs i64) (* n (: 16 i64))) <ptr u64>))

      (var i i64)
      (= i (: 0 i64))
      (while (< i n)
        (block
          (var row = (index row_ids i))
          (= pairs [(* i (: 2 i64))]
             (call sr_make_prefix store column_idx row))
          (= pairs [(+ (* i (: 2 i64)) (: 1 i64))] (cast row u64))
          (= i (+ i (: 1 i64)))))

      ;; Two passes per step move the pairs to pair_work and back.
      (var digit = (: 0 i64))
      (while (< digit (: 64 i64))
        (block
          (call sr_pair_count_pass pairs pair_work counts n digit desc)
          (call sr_pair_count_pass pair_work pairs counts n (+ digit (: 8 i64)) desc)
          (= digit (+ digit (: 16 i64)))))

      (call sr_pair_tiebreak_runs store column_idx
        pairs pair_work (: 0 i64) n desc)

      (call sr_pairs_to_row_ids pairs row_ids n)))

  ;; Nullable stable counting pass from pairs into work: bucket 0 or 256 holds
  ;; nulls per nulls_first (mirrors radix_nullable.oz).
  (fun sr_pair_count_pass_nullable
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var pairs <ptr u64>)
        (var work <ptr u64>)
        (var counts <ptr u32>)
        (var n i64)
        (var digit i64)
        (var desc bool)
        (var nulls_first bool))
    (block
      (var i i64)
      (= i (: 0 i64))
      (while (< i (: 257 i64))
        (block
          (= counts [i] (: 0 u32))
          (= i (+ i (: 1 i64)))))

      (= i (: 0 i64))
      (while (< i n)
        (block
          (var row = (cast (index pairs (+ (* i (: 2 i64)) (: 1 i64))) i64))
          (var bucket i64)
          (if (! (call sr_row_valid store row column_idx))
            (block
              (if nulls_first
                (block (= bucket (: 0 i64)))
                (block (= bucket (: 256 i64)))))
            (block
              (= bucket (cast (& (>> (index pairs (* i (: 2 i64)))
                                     (cast digit u64))
                                 (: 255 u64)) i64))
              (if desc
                (block (= bucket (- (: 255 i64) bucket))))
              (if nulls_first
                (block (= bucket (+ bucket (: 1 i64)))))))
          (= counts [bucket] (+ (index counts bucket) (: 1 u32)))
          (= i (+ i (: 1 i64)))))

      (= i (: 1 i64))
      (while (< i (: 257 i64))
        (block
          (= counts [i] (+ (index counts i) (index counts (- i (: 1 i64)))))
          (= i (+ i (: 1 i64)))))

      (= i n)
      (while (> i (: 0 i64))
        (block
          (= i (- i (: 1 i64)))
          (var row = (cast (index pairs (+ (* i (: 2 i64)) (: 1 i64))) i64))
          (var bucket i64)
          (if (! (call sr_row_valid store row column_idx))
            (block
              (if nulls_first
                (block (= bucket (: 0 i64)))
                (block (= bucket (: 256 i64)))))
            (block
              (= bucket (cast (& (>> (index pairs (* i (: 2 i64)))
                                     (cast digit u64))
                                 (: 255 u64)) i64))
              (if desc
                (block (= bucket (- (: 255 i64) bucket))))
              (if nulls_first
                (block (= bucket (+ bucket (: 1 i64)))))))
          (var place = (cast (- (index counts bucket) (: 1 u32)) i64))
          (= work [(* place (: 2 i64))] (index pairs (* i (: 2 i64))))
          (= work [(+ (* place (: 2 i64)) (: 1 i64))]
             (index pairs (+ (* i (: 2 i64)) (: 1 i64))))
          (= counts [bucket] (cast place u32))))))

  ;; Nullable entry: nulls are grouped at the chosen end by the passes; the
  ;; tie-break runs only over the contiguous valid region.
  (fun sort_string_rowids_nullable
       ((var store <ptr TRowSet>)
        (var column_idx i64)
        (var row_ids <ptr i64>)
        (var work <ptr u64>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool)
        (var nulls_first bool))
    (block
      (var pairs = work)
      (var pair_work = (cast (+ (cast pairs i64) (* n (: 16 i64))) <ptr u64>))

      (var i i64)
      (= i (: 0 i64))
      (while (< i n)
        (block
          (var row = (index row_ids i))
          (var prefix = (: 0 u64))
          (if (call sr_row_valid store row column_idx)
            (block
              (= prefix (call sr_make_prefix store column_idx row))))
          (= pairs [(* i (: 2 i64))] prefix)
          (= pairs [(+ (* i (: 2 i64)) (: 1 i64))] (cast row u64))
          (= i (+ i (: 1 i64)))))

      ;; Two passes per step move the pairs to pair_work and back.
      (var digit = (: 0 i64))
      (while (< digit (: 64 i64))
        (block
          (call sr_pair_count_pass_nullable
            store column_idx pairs pair_work counts n digit desc nulls_first)
          (call sr_pair_count_pass_nullable
            store column_idx pair_work pairs counts n (+ digit (: 8 i64)) desc nulls_first)
          (= digit (+ digit (: 16 i64)))))

      (var lo = (: 0 i64))
      (var hi = n)
      (var scanning = #t)
      (while (&& (< lo hi) scanning)
        (block
          (if (! (call sr_row_valid store
                    (cast (index pairs (+ (* lo (: 2 i64)) (: 1 i64))) i64)
                    column_idx))
            (block (= lo (+ lo (: 1 i64))))
            (block (= scanning #f)))))
      (= scanning #t)
      (while (&& (< lo hi) scanning)
        (block
          (if (! (call sr_row_valid store
                    (cast (index pairs (+ (* (- hi (: 1 i64)) (: 2 i64)) (: 1 i64))) i64)
                    column_idx))
            (block (= hi (- hi (: 1 i64))))
            (block (= scanning #f)))))
      (call sr_pair_tiebreak_runs store column_idx
        pairs pair_work lo hi desc)

      (call sr_pairs_to_row_ids pairs row_ids n)))
)
