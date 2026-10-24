(block
  (pragma language overloads)

  ;; Default residual join filter: always passes. jt_emit_and_insert calls this
  ;; before pushing each matched pair. When a query has a residual predicate the
  ;; compiler replaces this definition with a generated one (gen.cpp,
  ;; GenJoinResidualFilterAst) that reads columns from the two row stores by
  ;; decoding the packed row IDs. If one row ID has batch index -1, the generated
  ;; filter reads that side from the corresponding stream_*_batch pointer, and
  ;; builds the strings it compares in `arena`. The parameters are unused here.
  (fun jt_residual_filter ((var left_store <ptr TRowSet>)
                           (var right_store <ptr TRowSet>)
                           (var arena <ptr i8>)
                           (var stream_left_batch <ref TRowSet>)
                           (var stream_right_batch <ref TRowSet>)
                           (var left_row_id i64)
                           (var right_row_id i64)) -> bool
    (block (return #t))))
