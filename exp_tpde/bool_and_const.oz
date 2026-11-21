; TPDE leaks the assignment of a phi fed by a constant branch (cmp imm).
(block
  (type key <struct (valid bool) (v i64)>)

  (fun <main> () -> i64
    (block
      (var k key)
      (= k (: (struct ((valid #t) (v (: 7 i64)))) key))
      (if (call key_valid k) (block (return 1)))
      (return 0)))

  (fun key_valid ((var k key)) -> bool
    (block (return (&& #t (field k valid))))))
