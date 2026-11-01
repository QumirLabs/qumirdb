(block
  ;; The symmetric hash join reuses the aggregation Robin Hood HashTable
  ;; (modules/qumirdb.cpp) as its hash map, so the probe table and lifecycle
  ;; (aht_init / aht_rehash) are NOT duplicated here. The dense
  ;; per-slot RowId bucket is stored in the generic AggBuffers as three int64
  ;; "columns" (NumAggs = 3):
  ;;   AggBuffers[0][slot] = bucket length
  ;;   AggBuffers[1][slot] = bucket capacity
  ;;   AggBuffers[2][slot] = heap RowId array pointer (stored as i64)
  ;; aht_init zeroes these, so empty buckets (len=0, cap=0, data=null) come for
  ;; free; aht_rehash copies the int64 values, which carries the bucket pointers
  ;; without recopying their contents.

  ;; jt_init: a HashTable specialized for joins (3 dense int64 columns).
  (fun jt_init ((var ht <ref HashTable>) (var capacity i64) (var key_size i64)) -> bool
    (block
      (return (call aht_init ht capacity (: 3 i64) key_size))))

  ;; Appends row_id to the dense RowId bucket at dense slot `slot`
  ;; (amortized O(1), 1.5x growth from capacity 4). The slot's three columns must
  ;; already be zeroed (aht_init does this; new slots after rehash are zeroed by
  ;; jt_emit_and_insert before the first append).
  (fun jb_append ((var ht <ref HashTable>) (var slot i64) (var row_id i64)) -> bool
    (block
      (var aggs = (field ht AggBuffers))
      (var counts = (index aggs (: 0 i64)))
      (var caps = (index aggs (: 1 i64)))
      (var datas = (index aggs (: 2 i64)))
      (var count = (index counts slot))
      (var capacity = (index caps slot))
      (var data = (cast (index datas slot) <ptr i64>))
      (if (== count capacity)
        (block
          (var new_cap i64)
          (if (== capacity (: 0 i64))
            (block (= new_cap (: 4 i64)))
            (block (= new_cap (+ capacity (/ capacity (: 2 i64))))))
          (var new_data =
            (cast (call qdb_realloc (cast data <ptr i8>) (* count (: 8 i64)) (* new_cap (: 8 i64))) <ptr i64>))
          (if (== (cast new_data i64) (: 0 i64)) (block (return #f)))
          (= data new_data)
          (= capacity new_cap)
          (= caps [slot] capacity)
          (= datas [slot] (cast data i64))))
      (= data [count] row_id)
      (= counts [slot] (+ count (: 1 i64)))
      (return #t)))

  ;; ---- output pair buffer growth (2 i64 per pair, interleaved) ----

  (fun pb_push ((var buf <ref PairBuffer>)
                (var left_id i64)
                (var right_id i64)) -> bool
    (block
      (var count = (field buf Count))
      (var capacity = (field buf Capacity))
      (var data = (field buf Data))
      (if (== count capacity)
        (block
          (var new_cap i64)
          (if (== capacity (: 0 i64))
            (block (= new_cap (: 4 i64)))
            (block (= new_cap (+ capacity (/ capacity (: 2 i64))))))
          (var new_data =
            (cast (call qdb_realloc (cast data <ptr i8>) (* count (: 16 i64)) (* new_cap (: 16 i64))) <ptr i64>))
          (if (== (cast new_data i64) (: 0 i64)) (block (return #f)))
          (= data new_data)
          (= capacity new_cap)
          (field_assign buf Capacity capacity)
          (field_assign buf Data data)))
      (= data [(* count (: 2 i64))] left_id)
      (= data [(+ (* count (: 2 i64)) (: 1 i64))] right_id)
      (field_assign buf Count (+ count (: 1 i64)))
      (return #t))))
