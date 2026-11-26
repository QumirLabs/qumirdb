(block
  (pragma language overloads)

  ;; Window column fillers. Given a sorted row-id permutation (row_ids[start ..
  ;; start+n)) and the input store, compute one output column aligned to the
  ;; sorted order. Reuses the sort row-id helpers (sr_row_valid /
  ;; sr_load_fixed_key from sort_rowids.oz) and the sort materialize owner list
  ;; (sort_materialize_record_owner from materialize.oz).

  (fun window_init_i64_output
       ((var n i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)) -> <ptr i64>
    (block
      (var data = (cast (call qdb_alloc (* n (: 8 i64))) <ptr i64>))
      (call sort_materialize_record_owner out data_owner_idx (cast data <ptr i8>))
      (field_assign out_col Data (cast data <ptr i8>))
      (field_assign out_col DataBitOffset (: 0 i32))
      (field_assign out_col Mask (cast (: 0 i64) <ptr u8>))
      (field_assign out_col MaskBitOffset (: 0 i32))
      (field_assign out_col Offsets (cast (: 0 i64) <ptr i64>))
      (field_assign out_col OffsetWidth (: 0 u8))
      (return data)))

  (fun window_init_fixed_output [Value]
       ((var n i64)
        (var value_bytes i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var witness <ptr Value>)) -> <ptr Value>
    (block
      (var data = (cast (call qdb_alloc (* n value_bytes)) <ptr Value>))
      (call sort_materialize_record_owner out data_owner_idx (cast data <ptr i8>))
      (field_assign out_col Data (cast data <ptr i8>))
      (field_assign out_col DataBitOffset (: 0 i32))
      (field_assign out_col Mask (cast (: 0 i64) <ptr u8>))
      (field_assign out_col MaskBitOffset (: 0 i32))
      (field_assign out_col Offsets (cast (: 0 i64) <ptr i64>))
      (field_assign out_col OffsetWidth (: 0 u8))
      (return data)))

  (fun window_init_masked_fixed_output [Value]
       ((var n i64)
        (var value_bytes i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var mask_owner_idx i64)
        (var witness <ptr Value>)) -> <ptr Value>
    (block
      (var data = (cast (call qdb_alloc (* n value_bytes)) <ptr Value>))
      (var mask_bytes = (call sort_materialize_mask_bytes n))
      (var mask =
        (cast (call qdb_alloc mask_bytes) <ptr u8>))
      (var mask_i = (: 0 i64))
      (while (< mask_i mask_bytes)
        (block
          (= mask [mask_i] (: 0 u8))
          (= mask_i (+ mask_i (: 1 i64)))))
      (call sort_materialize_record_owner out data_owner_idx (cast data <ptr i8>))
      (call sort_materialize_record_owner out mask_owner_idx (cast mask <ptr i8>))
      (field_assign out_col Data (cast data <ptr i8>))
      (field_assign out_col DataBitOffset (: 0 i32))
      (field_assign out_col Mask mask)
      (field_assign out_col MaskBitOffset (: 0 i32))
      (field_assign out_col Offsets (cast (: 0 i64) <ptr i64>))
      (field_assign out_col OffsetWidth (: 0 u8))
      (return data)))

  ;; sum(x) OVER (PARTITION BY ... ORDER BY ...) with frame UNBOUNDED PRECEDING ..
  ;; CURRENT ROW. peer_inclusive selects RANGE mode: CURRENT ROW then includes all
  ;; peer rows (equal order key), so every peer shares the running sum through the
  ;; last peer; peer_inclusive=false is ROWS mode (strict row-by-row). NULL args
  ;; are skipped (SQL sum ignores nulls); output is NULL until the first non-NULL.
  (fun window_fill_prefix_sum_fixed [Value Sum]
       ((var store <ptr TRowSet>)
        (var row_ids <ptr i64>)
        (var start i64)
        (var n i64)
        (var arg_col i64)
        (var arg_witness <ptr Value>)
        (var sum_witness <ptr Sum>)
        (var sum_bytes i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var mask_owner_idx i64)
        (var peer_inclusive bool))
    (block
      (var data =
        (call window_init_masked_fixed_output
          n sum_bytes out_col out data_owner_idx mask_owner_idx sum_witness))
      (var mask = (field out_col Mask))
      (var zero = (cast (: 0 i64) Sum))
      (var acc = zero)
      (var valid_count = (: 0 i64))
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (var row_i = (index row_ids (+ start i)))
          (if (> i (: 0 i64))
            (block
              (var prev = (index row_ids (+ start (- i (: 1 i64)))))
              (if (! (call window_same_partition store prev row_i))
                (block (= acc zero) (= valid_count (: 0 i64))))))
          (var peer_end = (+ i (: 1 i64)))
          (if peer_inclusive
            (block
              (while (&& (< peer_end n)
                         (&& (call window_same_partition store row_i
                                (index row_ids (+ start peer_end)))
                             (call window_same_order store row_i
                                (index row_ids (+ start peer_end)))))
                (block (= peer_end (+ peer_end (: 1 i64)))))))
          (var j = i)
          (while (< j peer_end)
            (block
              (var row_j = (index row_ids (+ start j)))
              (if (call sr_row_valid store row_j arg_col)
                (block
                  (= acc (+ acc
                    (cast
                      (call sr_load_fixed_key store row_j arg_col arg_witness)
                      Sum)))
                  (= valid_count (+ valid_count (: 1 i64)))))
              (= j (+ j (: 1 i64)))))
          (var valid = (> valid_count (: 0 i64)))
          (= j i)
          (while (< j peer_end)
            (block
              (call qdb_bitmap_set_valid mask j valid)
              (= data [j] acc)
              (= j (+ j (: 1 i64)))))
          (= i peer_end)))))

  ;; A decimal sum is DECIMAL(38, s): the argument, stored as i64 or BinInt (Value),
  ;; accumulates in i128.
  (fun window_fill_prefix_sum_binint [Value]
       ((var store <ptr TRowSet>)
        (var row_ids <ptr i64>)
        (var start i64)
        (var n i64)
        (var arg_col i64)
        (var arg_witness <ptr Value>)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var mask_owner_idx i64)
        (var peer_inclusive bool))
    (block
      (var witness = (cast (: 0 i64) <ptr BinInt>))
      (var data =
        (call window_init_masked_fixed_output
          n (: 16 i64) out_col out data_owner_idx mask_owner_idx witness))
      (var mask = (field out_col Mask))
      (var zero = (cast (struct ((V (: 0 i128)))) BinInt))
      (var acc = zero)
      (var valid_count = (: 0 i64))
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (var row_i = (index row_ids (+ start i)))
          (if (> i (: 0 i64))
            (block
              (var prev = (index row_ids (+ start (- i (: 1 i64)))))
              (if (! (call window_same_partition store prev row_i))
                (block (= acc zero) (= valid_count (: 0 i64))))))
          (var peer_end = (+ i (: 1 i64)))
          (if peer_inclusive
            (block
              (while (&& (< peer_end n)
                         (&& (call window_same_partition store row_i
                                (index row_ids (+ start peer_end)))
                             (call window_same_order store row_i
                                (index row_ids (+ start peer_end)))))
                (block (= peer_end (+ peer_end (: 1 i64)))))))
          (var j = i)
          (while (< j peer_end)
            (block
              (var row_j = (index row_ids (+ start j)))
              (if (call sr_row_valid store row_j arg_col)
                (block
                  (= acc (+ acc (call qdb_decimal_wide
                    (call sr_load_fixed_key store row_j arg_col arg_witness))))
                  (= valid_count (+ valid_count (: 1 i64)))))
              (= j (+ j (: 1 i64)))))
          (var valid = (> valid_count (: 0 i64)))
          (= j i)
          (while (< j peer_end)
            (block
              (call qdb_bitmap_set_valid mask j valid)
              (= data [j] acc)
              (= j (+ j (: 1 i64)))))
          (= i peer_end)))))

  (fun window_fill_prefix_max_fixed [Value]
       ((var store <ptr TRowSet>)
        (var row_ids <ptr i64>)
        (var start i64)
        (var n i64)
        (var arg_col i64)
        (var arg_witness <ptr Value>)
        (var value_bytes i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var mask_owner_idx i64)
        (var peer_inclusive bool))
    (block
      (var data =
        (call window_init_masked_fixed_output
          n value_bytes out_col out data_owner_idx mask_owner_idx arg_witness))
      (var mask = (field out_col Mask))
      (var zero = (cast (: 0 i64) Value))
      (var acc = zero)
      (var valid_count = (: 0 i64))
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (var row_i = (index row_ids (+ start i)))
          (if (> i (: 0 i64))
            (block
              (var prev = (index row_ids (+ start (- i (: 1 i64)))))
              (if (! (call window_same_partition store prev row_i))
                (block (= acc zero) (= valid_count (: 0 i64))))))
          (var peer_end = (+ i (: 1 i64)))
          (if peer_inclusive
            (block
              (while (&& (< peer_end n)
                         (&& (call window_same_partition store row_i
                                (index row_ids (+ start peer_end)))
                             (call window_same_order store row_i
                                (index row_ids (+ start peer_end)))))
                (block (= peer_end (+ peer_end (: 1 i64)))))))
          (var j = i)
          (while (< j peer_end)
            (block
              (var row_j = (index row_ids (+ start j)))
              (if (call sr_row_valid store row_j arg_col)
                (block
                  (var value = (call sr_load_fixed_key store row_j arg_col arg_witness))
                  (if (== valid_count (: 0 i64))
                    (block (= acc value))
                    (block (if (> value acc) (block (= acc value)))))
                  (= valid_count (+ valid_count (: 1 i64)))))
              (= j (+ j (: 1 i64)))))
          (var valid = (> valid_count (: 0 i64)))
          (= j i)
          (while (< j peer_end)
            (block
              (call qdb_bitmap_set_valid mask j valid)
              (= data [j] acc)
              (= j (+ j (: 1 i64)))))
          (= i peer_end)))))

  (fun window_fill_prefix_max_binint
       ((var store <ptr TRowSet>)
        (var row_ids <ptr i64>)
        (var start i64)
        (var n i64)
        (var arg_col i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var mask_owner_idx i64)
        (var peer_inclusive bool))
    (block
      (var witness = (cast (: 0 i64) <ptr BinInt>))
      (var data =
        (call window_init_masked_fixed_output
          n (: 16 i64) out_col out data_owner_idx mask_owner_idx witness))
      (var mask = (field out_col Mask))
      (var zero = (cast (struct ((V (: 0 i128)))) BinInt))
      (var acc = zero)
      (var valid_count = (: 0 i64))
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (var row_i = (index row_ids (+ start i)))
          (if (> i (: 0 i64))
            (block
              (var prev = (index row_ids (+ start (- i (: 1 i64)))))
              (if (! (call window_same_partition store prev row_i))
                (block (= acc zero) (= valid_count (: 0 i64))))))
          (var peer_end = (+ i (: 1 i64)))
          (if peer_inclusive
            (block
              (while (&& (< peer_end n)
                         (&& (call window_same_partition store row_i
                                (index row_ids (+ start peer_end)))
                             (call window_same_order store row_i
                                (index row_ids (+ start peer_end)))))
                (block (= peer_end (+ peer_end (: 1 i64)))))))
          (var j = i)
          (while (< j peer_end)
            (block
              (var row_j = (index row_ids (+ start j)))
              (if (call sr_row_valid store row_j arg_col)
                (block
                  (var value = (call sr_load_fixed_key store row_j arg_col witness))
                  (if (== valid_count (: 0 i64))
                    (block (= acc value))
                    (block (if (> value acc) (block (= acc value)))))
                  (= valid_count (+ valid_count (: 1 i64)))))
              (= j (+ j (: 1 i64)))))
          (var valid = (> valid_count (: 0 i64)))
          (= j i)
          (while (< j peer_end)
            (block
              (call qdb_bitmap_set_valid mask j valid)
              (= data [j] acc)
              (= j (+ j (: 1 i64)))))
          (= i peer_end)))))

  ;; rank() OVER (PARTITION BY optional_i64 ORDER BY fixed-width). Peers with
  ;; the same order key keep the same rank; the next distinct peer gets its
  ;; 1-based row position inside the partition. rows_before rows of the first
  ;; partition precede the slice in other ranges (the range scheme's carry).
  (fun window_fill_rank
       ((var store <ptr TRowSet>)
        (var row_ids <ptr i64>)
        (var start i64)
        (var n i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var rows_before i64))
    (block
      (var data = (call window_init_i64_output n out_col out data_owner_idx))
      (var rank = (+ rows_before 1))
      (var pos = (+ rows_before 1))
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (var row_id = (index row_ids (+ start i)))
          (if (> i (: 0 i64))
            (block
              (var prev_row_id = (index row_ids (+ start (- i (: 1 i64)))))
              (if (! (call window_same_partition
                        store prev_row_id row_id))
                (block
                  (= rank (: 1 i64))
                  (= pos (: 1 i64)))
                (block
                  (= pos (+ pos (: 1 i64)))
                  (if (! (call window_same_order
                            store prev_row_id row_id))
                    (block (= rank pos)))))))
          (= data [i] rank)
          (= i (+ i (: 1 i64)))))))

  ;; row_number() OVER (PARTITION BY ... [ORDER BY ...]): the 1-based row
  ;; position inside the partition; peers get distinct numbers. rows_before as
  ;; in window_fill_rank.
  (fun window_fill_row_number
       ((var store <ptr TRowSet>)
        (var row_ids <ptr i64>)
        (var start i64)
        (var n i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var rows_before i64))
    (block
      (var data = (call window_init_i64_output n out_col out data_owner_idx))
      (var pos = (+ rows_before 1))
      (var i = 0)
      (while (< i n)
        (block
          (if (> i 0)
            (block
              (if (call window_same_partition
                    store (index row_ids (+ start (- i 1)))
                    (index row_ids (+ start i)))
                (block (= pos (+ pos 1)))
                (block (= pos 1)))))
          (= data [i] pos)
          (= i (+ i 1))))))

  ;; avg(x) OVER (PARTITION BY ...) with no ORDER BY: compute one value for the
  ;; whole contiguous partition and broadcast it to every row in that partition.
  (fun window_fill_partition_avg_f64 [Value]
       ((var store <ptr TRowSet>)
        (var row_ids <ptr i64>)
        (var start i64)
        (var n i64)
        (var arg_col i64)
        (var arg_witness <ptr Value>)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var mask_owner_idx i64))
    (block
      (var out_witness = (cast (: 0 i64) <ptr f64>))
      (var data =
        (call window_init_masked_fixed_output
          n (: 8 i64) out_col out data_owner_idx mask_owner_idx out_witness))
      (var mask = (field out_col Mask))
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (var first_row_id = (index row_ids (+ start i)))
          (var run_end = (+ i (: 1 i64)))
          (while (&& (< run_end n)
                     (call window_same_partition
                       store first_row_id (index row_ids (+ start run_end))))
            (block (= run_end (+ run_end (: 1 i64)))))

          (var acc = (: 0.0 f64))
          (var count = (: 0 i64))
          (var j = i)
          (while (< j run_end)
            (block
              (var row_id = (index row_ids (+ start j)))
              (if (call sr_row_valid store row_id arg_col)
                (block
                  (= acc (+ acc
                    (cast (call sr_load_fixed_key
                      store row_id arg_col arg_witness) f64)))
                  (= count (+ count (: 1 i64)))))
              (= j (+ j (: 1 i64)))))

          (var avg = (: 0.0 f64))
          (if (> count (: 0 i64))
            (block (= avg (/ acc (cast count f64)))))
          (= j i)
          (while (< j run_end)
            (block
              (call qdb_bitmap_set_valid mask j (> count (: 0 i64)))
              (= data [j] avg)
              (= j (+ j (: 1 i64)))))
          (= i run_end)))))

  ;; avg(x) OVER (PARTITION BY ...) for decimal: the sum is scaled up by
  ;; 10^scale_delta before dividing by the count so the quotient keeps
  ;; fractional precision instead of truncating to the argument's scale. It is
  ;; computed in i128 and stored in the result's storage (Out).
  (fun window_fill_partition_avg_binint [Value Out]
       ((var store <ptr TRowSet>)
        (var row_ids <ptr i64>)
        (var start i64)
        (var n i64)
        (var arg_col i64)
        (var arg_witness <ptr Value>)
        (var out_witness <ptr Out>)
        (var out_bytes i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var mask_owner_idx i64)
        (var scale_delta i64))
    (block
      (var data =
        (call window_init_masked_fixed_output
          n out_bytes out_col out data_owner_idx mask_owner_idx out_witness))
      (var mask = (field out_col Mask))
      (var zero = (cast (struct ((V (: 0 i128)))) BinInt))
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (var first_row_id = (index row_ids (+ start i)))
          (var run_end = (+ i (: 1 i64)))
          (while (&& (< run_end n)
                     (call window_same_partition
                       store first_row_id (index row_ids (+ start run_end))))
            (block (= run_end (+ run_end (: 1 i64)))))

          (var acc = zero)
          (var count = (: 0 i64))
          (var j = i)
          (while (< j run_end)
            (block
              (var row_id = (index row_ids (+ start j)))
              (if (call sr_row_valid store row_id arg_col)
                (block
                  (= acc (+ acc (call qdb_decimal_wide
                    (call sr_load_fixed_key store row_id arg_col arg_witness))))
                  (= count (+ count (: 1 i64)))))
              (= j (+ j (: 1 i64)))))

          (var avg = zero)
          (if (> count (: 0 i64))
            (block
              (= avg (/ (call qdb_decimal_scale_up acc scale_delta) count))))
          (= j i)
          (while (< j run_end)
            (block
              (call qdb_bitmap_set_valid mask j (> count (: 0 i64)))
              (= data [j] (call qdb_decimal_as avg out_witness))
              (= j (+ j (: 1 i64)))))
          (= i run_end)))))

  (fun window_fill_partition_sum_fixed [Value Sum]
       ((var store <ptr TRowSet>)
        (var row_ids <ptr i64>)
        (var start i64)
        (var n i64)
        (var arg_col i64)
        (var arg_witness <ptr Value>)
        (var sum_witness <ptr Sum>)
        (var sum_bytes i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var mask_owner_idx i64))
    (block
      (var data =
        (call window_init_masked_fixed_output
          n sum_bytes out_col out data_owner_idx mask_owner_idx sum_witness))
      (var mask = (field out_col Mask))
      (var zero = (cast (: 0 i64) Sum))
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (var first_row_id = (index row_ids (+ start i)))
          (var run_end = (+ i (: 1 i64)))
          (while (&& (< run_end n)
                     (call window_same_partition
                       store first_row_id (index row_ids (+ start run_end))))
            (block (= run_end (+ run_end (: 1 i64)))))

          (var acc = zero)
          (var count = (: 0 i64))
          (var j = i)
          (while (< j run_end)
            (block
              (var row_id = (index row_ids (+ start j)))
              (if (call sr_row_valid store row_id arg_col)
                (block
                  (= acc (+ acc
                    (cast
                      (call sr_load_fixed_key store row_id arg_col arg_witness)
                      Sum)))
                  (= count (+ count (: 1 i64)))))
              (= j (+ j (: 1 i64)))))

          (= j i)
          (while (< j run_end)
            (block
              (call qdb_bitmap_set_valid mask j (> count (: 0 i64)))
              (= data [j] acc)
              (= j (+ j (: 1 i64)))))
          (= i run_end)))))

  (fun window_fill_partition_sum_binint [Value]
       ((var store <ptr TRowSet>)
        (var row_ids <ptr i64>)
        (var start i64)
        (var n i64)
        (var arg_col i64)
        (var arg_witness <ptr Value>)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var mask_owner_idx i64))
    (block
      (var witness = (cast (: 0 i64) <ptr BinInt>))
      (var data =
        (call window_init_masked_fixed_output
          n (: 16 i64) out_col out data_owner_idx mask_owner_idx witness))
      (var mask = (field out_col Mask))
      (var zero = (cast (struct ((V (: 0 i128)))) BinInt))
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (var first_row_id = (index row_ids (+ start i)))
          (var run_end = (+ i (: 1 i64)))
          (while (&& (< run_end n)
                     (call window_same_partition
                       store first_row_id (index row_ids (+ start run_end))))
            (block (= run_end (+ run_end (: 1 i64)))))

          (var acc = zero)
          (var count = (: 0 i64))
          (var j = i)
          (while (< j run_end)
            (block
              (var row_id = (index row_ids (+ start j)))
              (if (call sr_row_valid store row_id arg_col)
                (block
                  (= acc (+ acc (call qdb_decimal_wide
                    (call sr_load_fixed_key store row_id arg_col arg_witness))))
                  (= count (+ count (: 1 i64)))))
              (= j (+ j (: 1 i64)))))

          (= j i)
          (while (< j run_end)
            (block
              (call qdb_bitmap_set_valid mask j (> count (: 0 i64)))
              (= data [j] acc)
              (= j (+ j (: 1 i64)))))
          (= i run_end)))))

  ;; Range scheme (wiki Window-parallelism). The splitters are row ids of sorted
  ;; sample rows in `store`; range j holds the keys in [splitter j-1, splitter j).

  ;; Routes the selected rows of store[batch]: row_ids[offsets[j] ..
  ;; offsets[j+1]) are the rows of range j (offsets has nsplit + 2 slots). tails[j]
  ;; counts the rows of range j in the partition of splitter j, which continues
  ;; into the next range. ranges is scratch, one slot per row.
  (fun qdb_window_range_route
       ((var store <ptr TRowSet>)
        (var splitters <ptr i64>)
        (var nsplit i64)
        (var batch i64)
        (var ranges <ptr i64>)
        (var offsets <ptr i64>)
        (var row_ids <ptr i64>)
        (var tails <ptr i64>)) -> i64
    (block
      (var rs = (index store batch))
      (var rows = (field rs RowCount))
      (var selection = (field rs Selection))
      (var has_selection = (!= (cast selection i64) 0))
      (var j = 0)
      (while (<= j (+ nsplit 1))
        (block
          (= offsets [j] 0)
          (= j (+ j 1))))
      (var r = 0)
      (while (< r rows)
        (block
          (var target = -1)
          (var selected = #t)
          (if has_selection
            (block (= selected (!= (index selection r) (: 0 u8)))))
          (if selected
            (block
              (var row_id = (| (<< batch 32) r))
              (var lo = 0)
              (var hi = nsplit)
              (while (< lo hi)
                (block
                  (var mid = (>> (+ lo hi) 1))
                  (if (call window_key_less store row_id (index splitters mid))
                    (block (= hi mid))
                    (block (= lo (+ mid 1))))))
              (= target lo)
              (= offsets [(+ target 1)] (+ (index offsets (+ target 1)) 1))
              (if (< target nsplit)
                (block
                  (if (call window_same_partition store row_id (index splitters target))
                    (block (= tails [target] (+ (index tails target) 1))))))))
          (= ranges [r] target)
          (= r (+ r 1))))
      (= j 0)
      (while (<= j nsplit)
        (block
          (= offsets [(+ j 1)] (+ (index offsets (+ j 1)) (index offsets j)))
          (= j (+ j 1))))
      (var routed = (index offsets (+ nsplit 1)))
      (= r 0)
      (while (< r rows)
        (block
          (var target = (index ranges r))
          (if (>= target 0)
            (block
              (var at = (index offsets target))
              (= row_ids [at] (| (<< batch 32) r))
              (= offsets [target] (+ at 1))))
          (= r (+ r 1))))
      ;; The placement advanced each start to the next range's start.
      (= j nsplit)
      (while (> j 0)
        (block
          (= offsets [j] (index offsets (- j 1)))
          (= j (- j 1))))
      (= offsets [0] 0)
      (return routed)))

  ;; The partial carries of one lane: carries[j] (nsplit + 1 slots) counts the
  ;; routed rows of range j's leading partition (the partition of splitter j-1)
  ;; in ranges before j. Peers never cross a range, so that is the row offset of
  ;; the range inside its leading partition.
  (fun qdb_window_range_carries
       ((var store <ptr TRowSet>)
        (var splitters <ptr i64>)
        (var nsplit i64)
        (var tails <ptr i64>)
        (var carries <ptr i64>)) -> i64
    (block
      (= carries [0] 0)
      (var j = 0)
      (while (< j nsplit)
        (block
          (var carry = (index tails j))
          (if (> j 0)
            (block
              (if (call window_same_partition store
                    (index splitters (- j 1)) (index splitters j))
                (block (= carry (+ carry (index carries j)))))))
          (= carries [(+ j 1)] carry)
          (= j (+ j 1))))
      (return (+ nsplit 1))))

  ;; Sums the partial carries of a range: one i64 column, one row per lane.
  (fun window_carry_rows_before
       ((var carries <ptr TRowSet>)
        (var carry_count i64)) -> i64
    (block
      (var total = 0)
      (var b = 0)
      (while (< b carry_count)
        (block
          (var rs = (index carries b))
          (var columns = (field rs Columns))
          (var col = (index columns 0))
          (var data = (cast (cast (field col Data) i64) <ptr i64>))
          (var r = 0)
          (while (< r (field rs RowCount))
            (block
              (= total (+ total (index data r)))
              (= r (+ r 1))))
          (= b (+ b 1))))
      (return total)))
)
