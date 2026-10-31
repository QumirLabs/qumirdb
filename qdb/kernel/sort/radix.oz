(block
  (pragma language overloads)

  (fun qumir_radix_key ((var value u8)) -> u64
    (block
      (return (cast value u64))))

  (fun qumir_radix_key ((var value i8)) -> u64
    (block
      (return (cast (^ (bitcast value u8) (<< (: 1 u8) (: 7 u8))) u64))))

  (fun qumir_radix_key ((var value u16)) -> u64
    (block
      (return (cast value u64))))

  (fun qumir_radix_key ((var value i16)) -> u64
    (block
      (return (cast (^ (bitcast value u16) (<< (: 1 u16) (: 15 u16))) u64))))

  (fun qumir_radix_key ((var value u32)) -> u64
    (block
      (return (cast value u64))))

  (fun qumir_radix_key ((var value i32)) -> u64
    (block
      (return (cast (^ (bitcast value u32) (<< (: 1 u32) (: 31 u32))) u64))))

  (fun qumir_radix_key ((var value u64)) -> u64
    (block
      (return value)))

  (fun qumir_radix_key ((var value i64)) -> u64
    (block
      (return (^ (bitcast value u64) (<< (: 1 u64) (: 63 u64))))))

  (fun qumir_radix_key ((var value f64)) -> u64
    (block
      (var bits = (bitcast value u64))
      (var mask = (<< (: 1 u64) (: 63 u64)))
      (if (!= (>> bits (: 63 u64)) (: 0 u64))
        (block
          (= mask (bitcast (: -1 i64) u64))))
      (return (^ bits mask))))

  (fun qumir_radix_key ((var value f32)) -> u64
    (block
      (var bits = (bitcast value u32))
      (var mask = (<< (: 1 u32) (: 31 u32)))
      (if (!= (>> bits (: 31 u32)) (: 0 u32))
        (block
          (= mask (bitcast (: -1 i32) u32))))
      (return (cast (^ bits mask) u64))))

  (fun qumir_radix_digit ((var key u64) (var digit i64)) -> u32
    (block
      (return (cast (& (>> key (cast digit u64)) (: 255 u64)) u32))))

  (fun radix_count_sort_indices [Value]
       ((var values <ptr Value>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
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
          (var row = (index indices i))
          (var bucket = (call qumir_radix_digit
            (call qumir_radix_key (index values (cast row i64))) digit))
          (if desc
            (block
              (= bucket (- (: 255 u32) bucket))))
          (= counts [(cast bucket i64)] (+ (index counts (cast bucket i64)) (: 1 u32)))
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
          (var row = (index indices i))
          (var bucket = (call qumir_radix_digit
            (call qumir_radix_key (index values (cast row i64))) digit))
          (if desc
            (block
              (= bucket (- (: 255 u32) bucket))))
          (var bucket_index = (cast bucket i64))
          (var place = (- (index counts bucket_index) (: 1 u32)))
          (= work [(cast place i64)] row)
          (= counts [bucket_index] place)))

      (= i (: 0 i64))
      (while (< i n)
        (block
          (= indices [i] (index work i))
          (= i (+ i (: 1 i64)))))))

  (fun radix_sort_indices [Value]
       ((var values <ptr Value>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var key_bits i64)
        (var desc bool))
    (block
      (var digit i64)
      (= digit (: 0 i64))
      (while (< digit key_bits)
        (block
          (call radix_count_sort_indices values indices work counts n digit desc)
          (= digit (+ digit (: 8 i64)))))))

;; do not use in hot path
(fun radix_sort [Value]
  ((var n i64)
   (var values <array Value 1>)
   (var desc bool)) -> void

  (block
    (var key_bits = (: 64 i64))
    (var counts <array u32 1> [0 255])
    (var work <array Value 1> [0 n])
    (var indices <array u32 1> [0 n])
    (var index_work <array u32 1> [0 n])

    (var i i64)
    (= i (: 0 i64))
    (while (< i n)
      (block
        (= indices [i] (cast i u32))
        (= i (+ i (: 1 i64)))))

    (call radix_sort_indices
      (cast values <ptr Value>)
      (cast indices <ptr u32>)
      (cast index_work <ptr u32>)
      (cast counts <ptr u32>)
      n
      key_bits
      desc)

    (= i (: 0 i64))
    (while (< i n)
      (block
        (var source = (cast (index indices i) i64))
        (var item = (index values source))
        (= work [i] item)
        (= i (+ i (: 1 i64)))))

    (= i (: 0 i64))
    (while (< i n)
      (block
        (= values [i] (index work i))
        (= i (+ i (: 1 i64)))))
  )
)

(fun radix_sort [Value]
  ((var n i64)
   (var values <array Value 1>)) -> void
   (block
    (call radix_sort n values #f))
)

(fun radix_sort_desc [Value]
  ((var n i64)
   (var values <array Value 1>)) -> void
   (block
    (call radix_sort n values #t))
)

) ; global block
