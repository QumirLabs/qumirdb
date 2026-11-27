(block
  (pragma language overloads)

  ;; Range scheme (wiki Window-parallelism). The splitters are row ids of sorted
  ;; sample rows in `store`; range j holds the keys in [splitter j-1, splitter j).

  ;; Routes the selected rows of store[batch]: row_ids[offsets[j] ..
  ;; offsets[j+1]) are the rows of range j (offsets has nsplit + 2 slots). Row j
  ;; of the tails table (window_range_tail_add) gathers the rows of range j in the
  ;; partition of splitter j, which continues into the next range. ranges is
  ;; scratch, one slot per row.
  (fun qdb_window_range_route
       ((var store <ptr TRowSet>)
        (var splitters <ptr i64>)
        (var nsplit i64)
        (var batch i64)
        (var ranges <ptr i64>)
        (var offsets <ptr i64>)
        (var row_ids <ptr i64>)
        (var tails <ptr TRowSet>)) -> i64
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
                    (block (call window_range_tail_add store row_id tails target)))))))
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

  ;; The partial carries of one lane: row j of the carries table (nsplit + 1
  ;; rows, row 0 empty) is the state of the routed rows of range j's leading
  ;; partition (the partition of splitter j-1) in ranges before j. Peers never
  ;; cross a range, so the row count is the row offset of the range inside its
  ;; leading partition.
  (fun qdb_window_range_carries
       ((var store <ptr TRowSet>)
        (var splitters <ptr i64>)
        (var nsplit i64)
        (var tails <ptr TRowSet>)
        (var carries <ptr TRowSet>)) -> i64
    (block
      (var j = 0)
      (while (< j nsplit)
        (block
          (var same = #f)
          (if (> j 0)
            (block
              (= same (call window_same_partition store
                (index splitters (- j 1)) (index splitters j)))))
          (call window_range_carry_step tails carries j same)
          (= j (+ j 1))))
      (return (+ nsplit 1))))
)
