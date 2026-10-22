(block
  ;; Nested-loop join: every selected left row is compared with every selected
  ;; row of the buffered right store, and a pair matches when
  ;; jt_residual_filter accepts it (the default accepts every pair, a cross
  ;; product). Matches and unmatched rows are emitted as canonical
  ;; (left_id, right_id) pairs into PairBuffer, -1 standing for the absent side;
  ;; materialization is shared with equi-join through jt_materialize. The
  ;; generated xj_dispatch fixes the emission flags of the join type.

  (fun xj_selected ((var batch <ref TRowSet>) (var row i64)) -> bool
    (block
      (var selection = (field batch Selection))
      (if (== (cast selection i64) (: 0 i64))
        (block (return #t)))
      (return (!= (index selection row) (: 0 u8)))))

  (fun xj_row_id ((var batch_idx i64) (var row i64)) -> i64
    (block
      (return (+ (<< batch_idx (: 32 i64)) (& row (: 4294967295 i64))))))

  ;; Joins the left batch `batch_idx` of left_store with the right store.
  ;; emit_pairs pushes every match (inner and outer joins). emit_matched and
  ;; emit_unmatched push the left row once with no right side when it has a
  ;; match (SEMI) or has none (ANTI, and the NULL padding of LEFT / FULL).
  ;; track_right flags each matched right row by its ordinal in the store, so
  ;; xj_emit_unmatched_right can pad the right rows no left row matched.
  (fun xj_emit ((var left_store <ptr TRowSet>)
                (var batch_idx i64)
                (var right_store <ptr TRowSet>)
                (var right_batch_count i64)
                (var right_matched <ptr u8>)
                (var pairs <ref PairBuffer>)
                (var arena <ptr i8>)
                (var emit_pairs bool)
                (var emit_matched bool)
                (var emit_unmatched bool)
                (var track_right bool)) -> bool
    (block
      (var batch = (index left_store batch_idx))
      ;; Without pairs or right flags, the first match decides the left row.
      (var first_match_decides = (&& (! emit_pairs) (! track_right)))
      (var l i64)
      (= l 0)
      (while (< l (field batch RowCount))
        (block
          (if (call xj_selected batch l)
            (block
              (var left_id = (call xj_row_id batch_idx l))
              (var matched = #f)
              (var scanning = #t)
              (var ordinal i64)
              (= ordinal 0)
              (var rb i64)
              (= rb 0)
              (while (&& scanning (< rb right_batch_count))
                (block
                  (var right_batch = (index right_store rb))
                  (var r i64)
                  (= r 0)
                  (while (&& scanning (< r (field right_batch RowCount)))
                    (block
                      (if (call xj_selected right_batch r)
                        (block
                          (var right_id = (call xj_row_id rb r))
                          (if (call jt_residual_filter left_store right_store arena
                                batch right_batch left_id right_id)
                            (block
                              (= matched #t)
                              (= scanning (! first_match_decides))
                              (if emit_pairs
                                (block
                                  (if (! (call pb_push pairs left_id right_id))
                                    (block (return #f)))))
                              (if track_right
                                (block (= right_matched [ordinal] (: 1 u8))))))))
                      (= ordinal (+ ordinal 1))
                      (= r (+ r 1))))
                  (= rb (+ rb 1))))
              (if (|| (&& matched emit_matched) (&& (! matched) emit_unmatched))
                (block
                  (if (! (call pb_push pairs left_id -1))
                    (block (return #f)))))))
          (= l (+ l 1))))
      (return #t)))

  ;; After the last left batch: pads every selected right row that no left
  ;; row matched with a NULL left side (RIGHT / FULL).
  (fun xj_emit_unmatched_right ((var right_store <ptr TRowSet>)
                                (var right_batch_count i64)
                                (var right_matched <ptr u8>)
                                (var pairs <ref PairBuffer>)) -> bool
    (block
      (var ordinal i64)
      (= ordinal 0)
      (var rb i64)
      (= rb 0)
      (while (< rb right_batch_count)
        (block
          (var right_batch = (index right_store rb))
          (var r i64)
          (= r 0)
          (while (< r (field right_batch RowCount))
            (block
              (if (&& (call xj_selected right_batch r)
                      (== (index right_matched ordinal) (: 0 u8)))
                (block
                  (if (! (call pb_push pairs -1 (call xj_row_id rb r)))
                    (block (return #f)))))
              (= ordinal (+ ordinal 1))
              (= r (+ r 1))))
          (= rb (+ rb 1))))
      (return #t))))
