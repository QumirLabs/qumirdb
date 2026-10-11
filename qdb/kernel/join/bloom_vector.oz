(block
  (fun bloom_mix4 ((var value <vec u64 4>)) -> <vec u64 4>
    (block
      (= value (^ value (>> value 30)))
      (= value (* value (cast -4658895280553007687 u64)))
      (= value (^ value (>> value 27)))
      (= value (* value (cast -7723592293110705685 u64)))
      (return (^ value (>> value 31)))))
  (fun bloom_probe4
       ((var words <ptr u64>) (var hashes <ptr u64>)
        (var output <ptr u8>) (var count i64) (var block_mask u64))
    (block
      (var row = 0)
      (while (< row count)
        (block
          (var hash = (vec (index hashes row)
                         (index hashes (+ row 1))
                         (index hashes (+ row 2))
                         (index hashes (+ row 3))))
          (var block = (* (& (call bloom_mix4 hash) block_mask) (cast 8 u64)))
          (var bits = (call bloom_mix4
                         (^ hash (cast -7046029254386353131 u64))))
          (var alive = (vec #t #t #t #t))
          (var word = 0)
          (while (&& (< word 8) (call builtin::any alive))
            (block
              (var found = (call builtin::gather words (+ block (cast word u64))))
              (var bit = (<< (cast 1 u64)
                           (& (>> bits (* word 8)) (cast 63 u64))))
              (= alive (& alive (!= (& found bit) (cast 0 u64))))
              (= word (+ word 1))))
          (= output[row] (cast (index alive 0) u8))
          (= output[(+ row 1)] (cast (index alive 1) u8))
          (= output[(+ row 2)] (cast (index alive 2) u8))
          (= output[(+ row 3)] (cast (index alive 3) u8))
          (= row (+ row 4)))))))
