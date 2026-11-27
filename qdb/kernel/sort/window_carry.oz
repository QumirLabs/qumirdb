(block
  (pragma language overloads)

  ;; The running states the range scheme of a window carries between key ranges
  ;; (wiki Window-parallelism). They live in tables with one row per range:
  ;; column 0 counts rows, and each running aggregate has its value in column c
  ;; and the count of its values in c + 1. Rows are packed row ids into the table.

  (fun window_store_fixed [Value]
       ((var store <ptr TRowSet>)
        (var row_id i64)
        (var column_idx i64)
        (var value Value))
    (block
      (var col = (call sr_column store row_id column_idx))
      (var data = (cast (cast (field col Data) i64) <ptr Value>))
      (= data [(call sr_row_index row_id)] value)))

  (fun window_carry_rows
       ((var table <ptr TRowSet>) (var row_id i64) (var rows i64))
    (block
      (var witness = (cast (: 0 i64) <ptr i64>))
      (call window_store_fixed table row_id 0
        (+ (call sr_load_fixed_key table row_id 0 witness) rows))))

  (fun window_carry_sum [Acc]
       ((var table <ptr TRowSet>)
        (var row_id i64)
        (var c i64)
        (var witness <ptr Acc>)
        (var value Acc)
        (var count i64))
    (block
      (var count_witness = (cast (: 0 i64) <ptr i64>))
      (call window_store_fixed table row_id c
        (+ (call sr_load_fixed_key table row_id c witness) value))
      (call window_store_fixed table row_id (+ c 1)
        (+ (call sr_load_fixed_key table row_id (+ c 1) count_witness) count))))

  ;; The larger of two maxima over acc_count and count values (none: no value).
  (fun window_max_merge [Acc]
       ((var acc Acc) (var acc_count i64) (var value Acc) (var count i64)) -> Acc
    (block
      (if (== count 0)
        (block (return acc)))
      (if (== acc_count 0)
        (block (return value)))
      (if (> value acc)
        (block (return value)))
      (return acc)))

  (fun window_carry_max [Acc]
       ((var table <ptr TRowSet>)
        (var row_id i64)
        (var c i64)
        (var witness <ptr Acc>)
        (var value Acc)
        (var count i64))
    (block
      (var count_witness = (cast (: 0 i64) <ptr i64>))
      (var held = (call sr_load_fixed_key table row_id (+ c 1) count_witness))
      (call window_store_fixed table row_id c
        (call window_max_merge
          (call sr_load_fixed_key table row_id c witness) held value count))
      (call window_store_fixed table row_id (+ c 1) (+ held count))))
)
