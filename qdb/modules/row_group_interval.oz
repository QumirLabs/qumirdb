(block
  (pragma language overloads)

  ;; Flags carried by every numeric range:
  ;;   1 = finite/orderable min/max are present
  ;;   2 = NULL is possible
  ;;   4 = a non-NULL value is possible
  ;;   8 = NaN is possible (floating-point ranges only)
  (type RgRange [T] <struct (Lo T) (Hi T) (Flags i64)>)

  ;; Truth is an i64 set of possible SQL results:
  ;;   1 = FALSE, 2 = TRUE, 4 = NULL.
  (fun rg_truth_unknown () -> i64
    (block (return 7)))
  (fun rg_truth_false () -> i64
    (block (return 1)))
  (fun rg_truth_true () -> i64
    (block (return 2)))

  (fun rg_truth_not ((var a i64)) -> i64
    (block
      (var out = (& a 4))
      (if (!= (& a 1) 0)
        (block (= out (| out 2))))
      (if (!= (& a 2) 0)
        (block (= out (| out 1))))
      (return out)))

  (fun rg_truth_and ((var a i64) (var b i64)) -> i64
    (block
      (var out = 0)
      (if (&& (!= (& a 2) 0)
               (!= (& b 2) 0))
        (block (= out (| out 2))))
      (if (|| (!= (& a 1) 0)
               (!= (& b 1) 0))
        (block (= out (| out 1))))
      (if (||
            (&& (!= (& a 4) 0)
                 (!= (& b 6) 0))
            (&& (!= (& b 4) 0)
                 (!= (& a 6) 0)))
        (block (= out (| out 4))))
      (return out)))

  (fun rg_truth_or ((var a i64) (var b i64)) -> i64
    (block
      (var out = 0)
      (if (|| (!= (& a 2) 0)
               (!= (& b 2) 0))
        (block (= out (| out 2))))
      (if (&& (!= (& a 1) 0)
               (!= (& b 1) 0))
        (block (= out (| out 1))))
      (if (||
            (&& (!= (& a 4) 0)
                 (!= (& b 5) 0))
            (&& (!= (& b 4) 0)
                 (!= (& a 5) 0)))
        (block (= out (| out 4))))
      (return out)))

  ;; qdb_is_true maps SQL NULL to FALSE.
  (fun rg_truth_is_true ((var a i64)) -> i64
    (block
      (var out = 0)
      (if (!= (& a 2) 0)
        (block (= out (| out 2))))
      (if (!= (& a 5) 0)
        (block (= out (| out 1))))
      (return out)))

  (fun rg_range [T] ((var lo T) (var hi T) (var flags i64)) -> <named RgRange [T]>
    (block
      (return (cast (struct ((Lo lo) (Hi hi) (Flags flags)))
                    <named RgRange [T]>))))

  (fun rg_const [T] ((var value T)) -> <named RgRange [T]>
    (block (return (call rg_range value value 5))))

  ;; A DECIMAL range is its unscaled i128; the VM receives it as two i64 words.
  (fun rg_i128_words ((var lo i64) (var hi i64)) -> i128
    (block
      (return (| (<< (cast hi i128) (: 64 i128))
                 (cast (cast lo u64) i128)))))

  (fun rg_i128_max () -> i128
    (block
      (return (- (: 0 i128) (+ (<< (: 1 i128) (: 127 i128)) (: 1 i128))))))

  (fun rg_i128_from_i64 ((var a <named RgRange [i64]>)) -> <named RgRange [i128]>
    (block
      (return (call rg_range (cast (field a Lo) i128) (cast (field a Hi) i128)
                             (field a Flags)))))

  (fun rg_i128_from_u64 ((var a <named RgRange [u64]>)) -> <named RgRange [i128]>
    (block
      (return (call rg_range (cast (field a Lo) i128) (cast (field a Hi) i128)
                             (field a Flags)))))

  ;; Multiplies an unscaled DECIMAL range by a power of ten to meet a larger
  ;; scale. Bounds that would leave i128 are dropped, which keeps the row group.
  (fun rg_i128_scale
       ((var a <named RgRange [i128]>) (var factor i128)) -> <named RgRange [i128]>
    (block
      (var flags = (& (field a Flags) 6))
      (var limit = (/ (call rg_i128_max) factor))
      (if (&& (call rg_has_bounds a)
               (<= (field a Lo) limit) (>= (field a Lo) (- (: 0 i128) limit))
               (<= (field a Hi) limit) (>= (field a Hi) (- (: 0 i128) limit)))
        (block
          (= flags (| flags 1))
          (return (call rg_range (* (field a Lo) factor) (* (field a Hi) factor) flags))))
      (return (call rg_range (: 0 i128) (: 0 i128) flags))))

  (fun rg_has_bounds [T] ((var a <named RgRange [T]>)) -> bool
    (block (return (!= (& (field a Flags) 1) 0))))
  (fun rg_may_null [T] ((var a <named RgRange [T]>)) -> bool
    (block (return (!= (& (field a Flags) 2) 0))))
  (fun rg_may_value [T] ((var a <named RgRange [T]>)) -> bool
    (block (return (!= (& (field a Flags) 4) 0))))
  (fun rg_may_nan [T] ((var a <named RgRange [T]>)) -> bool
    (block (return (!= (& (field a Flags) 8) 0))))

  (fun rg_null_value_flags [T]
       ((var a <named RgRange [T]>) (var b <named RgRange [T]>)) -> i64
    (block
      (var flags = 0)
      (if (|| (call rg_may_null a) (call rg_may_null b))
        (block (= flags (| flags 2))))
      (if (&& (call rg_may_value a) (call rg_may_value b))
        (block (= flags (| flags 4))))
      (return flags)))

  (fun rg_is_null [T] ((var a <named RgRange [T]>)) -> i64
    (block
      (var out = 0)
      (if (call rg_may_null a) (block (= out (| out 2))))
      (if (call rg_may_value a) (block (= out (| out 1))))
      (return out)))

  (fun rg_cmp_eq [T]
       ((var a <named RgRange [T]>) (var b <named RgRange [T]>)) -> i64
    (block
      (var out = 0)
      (if (|| (call rg_may_null a) (call rg_may_null b))
        (block (= out (| out 4))))
      (if (&& (call rg_may_value a) (call rg_may_value b))
        (block
          (if (&& (call rg_has_bounds a) (call rg_has_bounds b))
            (block
              (if (&& (<= (field a Lo) (field b Hi))
                       (<= (field b Lo) (field a Hi)))
                (block (= out (| out 2))))
              (if (|| (call rg_may_nan a)
                       (call rg_may_nan b)
                       (! (&& (== (field a Lo) (field a Hi))
                               (== (field b Lo) (field b Hi))
                               (== (field a Lo) (field b Lo)))))
                (block (= out (| out 1)))))
            (block (= out (| out 3))))))
      (return out)))

  (fun rg_cmp_ne [T]
       ((var a <named RgRange [T]>) (var b <named RgRange [T]>)) -> i64
    (block (return (call rg_truth_not (call rg_cmp_eq a b)))))

  (fun rg_cmp_lt [T]
       ((var a <named RgRange [T]>) (var b <named RgRange [T]>)) -> i64
    (block
      (var out = 0)
      (if (|| (call rg_may_null a) (call rg_may_null b))
        (block (= out (| out 4))))
      (if (&& (call rg_may_value a) (call rg_may_value b))
        (block
          (if (&& (call rg_has_bounds a) (call rg_has_bounds b))
            (block
              (if (< (field a Lo) (field b Hi))
                (block (= out (| out 2))))
              (if (|| (>= (field a Hi) (field b Lo))
                       (call rg_may_nan a) (call rg_may_nan b))
                (block (= out (| out 1)))))
            (block (= out (| out 3))))))
      (return out)))

  (fun rg_cmp_le [T]
       ((var a <named RgRange [T]>) (var b <named RgRange [T]>)) -> i64
    (block
      (var out = 0)
      (if (|| (call rg_may_null a) (call rg_may_null b))
        (block (= out (| out 4))))
      (if (&& (call rg_may_value a) (call rg_may_value b))
        (block
          (if (&& (call rg_has_bounds a) (call rg_has_bounds b))
            (block
              (if (<= (field a Lo) (field b Hi))
                (block (= out (| out 2))))
              (if (|| (> (field a Hi) (field b Lo))
                       (call rg_may_nan a) (call rg_may_nan b))
                (block (= out (| out 1)))))
            (block (= out (| out 3))))))
      (return out)))

  (fun rg_cmp_gt [T]
       ((var a <named RgRange [T]>) (var b <named RgRange [T]>)) -> i64
    (block (return (call rg_cmp_lt b a))))
  (fun rg_cmp_ge [T]
       ((var a <named RgRange [T]>) (var b <named RgRange [T]>)) -> i64
    (block (return (call rg_cmp_le b a))))

  (fun rg_i64_add_overflow ((var a i64) (var b i64) (var lo i64) (var hi i64)) -> bool
    (block
      (return (|| (&& (> b 0) (> a (- hi b)))
                  (&& (< b 0) (< a (- lo b)))))))

  (fun rg_i64_sub_overflow ((var a i64) (var b i64) (var lo i64) (var hi i64)) -> bool
    (block
      (return (|| (&& (> b 0) (< a (+ lo b)))
                  (&& (< b 0) (> a (+ hi b)))))))

  (fun rg_i64_mul_value_overflow
       ((var a i64) (var b i64) (var lo i64) (var hi i64)) -> bool
    (block
      ;; Check with division before multiplying; the VM never has to execute
      ;; an overflowing signed multiply.
      (if (> a 0)
        (block
          (if (> b 0) (block (return (> a (/ hi b)))))
          (if (< b 0) (block (return (< b (/ lo a)))))
          (return #f)))
      (if (< a 0)
        (block
          (if (> b 0) (block (return (< a (/ lo b)))))
          (if (< b 0) (block (return (< b (/ hi a)))))
          (return #f)))
      (return #f)))

  (fun rg_i64_add
       ((var a <named RgRange [i64]>) (var b <named RgRange [i64]>)
        (var type_lo i64) (var type_hi i64)) -> <named RgRange [i64]>
    (block
      (var flags = (call rg_null_value_flags a b))
      (if (&& (call rg_has_bounds a) (call rg_has_bounds b)
               (call rg_may_value a) (call rg_may_value b)
               (! (call rg_i64_add_overflow (field a Lo) (field b Lo) type_lo type_hi))
               (! (call rg_i64_add_overflow (field a Hi) (field b Hi) type_lo type_hi)))
        (block
          (= flags (| flags 1))
          (return (call rg_range (+ (field a Lo) (field b Lo))
                                 (+ (field a Hi) (field b Hi)) flags))))
      (return (call rg_range 0 0 flags))))

  (fun rg_i64_sub
       ((var a <named RgRange [i64]>) (var b <named RgRange [i64]>)
        (var type_lo i64) (var type_hi i64)) -> <named RgRange [i64]>
    (block
      (var flags = (call rg_null_value_flags a b))
      (if (&& (call rg_has_bounds a) (call rg_has_bounds b)
               (call rg_may_value a) (call rg_may_value b)
               (! (call rg_i64_sub_overflow (field a Lo) (field b Hi) type_lo type_hi))
               (! (call rg_i64_sub_overflow (field a Hi) (field b Lo) type_lo type_hi)))
        (block
          (= flags (| flags 1))
          (return (call rg_range (- (field a Lo) (field b Hi))
                                 (- (field a Hi) (field b Lo)) flags))))
      (return (call rg_range 0 0 flags))))

  (fun rg_i64_mul
       ((var a <named RgRange [i64]>) (var b <named RgRange [i64]>)
        (var type_lo i64) (var type_hi i64)) -> <named RgRange [i64]>
    (block
      (var flags = (call rg_null_value_flags a b))
      (if (&& (call rg_has_bounds a) (call rg_has_bounds b)
               (call rg_may_value a) (call rg_may_value b)
               (! (call rg_i64_mul_value_overflow (field a Lo) (field b Lo) type_lo type_hi))
               (! (call rg_i64_mul_value_overflow (field a Lo) (field b Hi) type_lo type_hi))
               (! (call rg_i64_mul_value_overflow (field a Hi) (field b Lo) type_lo type_hi))
               (! (call rg_i64_mul_value_overflow (field a Hi) (field b Hi) type_lo type_hi)))
        (block
          (var p0 = (* (field a Lo) (field b Lo)))
          (var p1 = (* (field a Lo) (field b Hi)))
          (var p2 = (* (field a Hi) (field b Lo)))
          (var p3 = (* (field a Hi) (field b Hi)))
          (var min_value = p0)
          (var max_value = p0)
          (if (< p1 min_value) (block (= min_value p1)))
          (if (< p2 min_value) (block (= min_value p2)))
          (if (< p3 min_value) (block (= min_value p3)))
          (if (> p1 max_value) (block (= max_value p1)))
          (if (> p2 max_value) (block (= max_value p2)))
          (if (> p3 max_value) (block (= max_value p3)))
          (= flags (| flags 1))
          (return (call rg_range min_value max_value flags))))
      (return (call rg_range 0 0 flags))))

  (fun rg_i64_neg
       ((var a <named RgRange [i64]>) (var type_lo i64) (var type_hi i64))
       -> <named RgRange [i64]>
    (block
      (var flags = (& (field a Flags) 6))
      (if (&& (call rg_has_bounds a) (call rg_may_value a)
               (!= (field a Lo) type_lo))
        (block
          (= flags (| flags 1))
          (return (call rg_range (- 0 (field a Hi))
                                 (- 0 (field a Lo)) flags))))
      (return (call rg_range 0 0 flags))))

  (fun rg_u64_add
       ((var a <named RgRange [u64]>) (var b <named RgRange [u64]>)
        (var type_hi u64)) -> <named RgRange [u64]>
    (block
      (var flags = (call rg_null_value_flags a b))
      (if (&& (call rg_has_bounds a) (call rg_has_bounds b)
               (call rg_may_value a) (call rg_may_value b)
               (<= (field a Lo) (- type_hi (field b Lo)))
               (<= (field a Hi) (- type_hi (field b Hi))))
        (block
          (= flags (| flags 1))
          (return (call rg_range (+ (field a Lo) (field b Lo))
                                 (+ (field a Hi) (field b Hi)) flags))))
      (return (call rg_range (: 0 u64) (: 0 u64) flags))))

  (fun rg_u64_sub
       ((var a <named RgRange [u64]>) (var b <named RgRange [u64]>))
       -> <named RgRange [u64]>
    (block
      (var flags = (call rg_null_value_flags a b))
      (if (&& (call rg_has_bounds a) (call rg_has_bounds b)
               (call rg_may_value a) (call rg_may_value b)
               (>= (field a Lo) (field b Hi)))
        (block
          (= flags (| flags 1))
          (return (call rg_range (- (field a Lo) (field b Hi))
                                 (- (field a Hi) (field b Lo)) flags))))
      (return (call rg_range (: 0 u64) (: 0 u64) flags))))

  (fun rg_u64_mul_overflow ((var a u64) (var b u64) (var hi u64)) -> bool
    (block
      (return (&& (!= b (: 0 u64)) (> a (/ hi b))))))

  (fun rg_u64_mul
       ((var a <named RgRange [u64]>) (var b <named RgRange [u64]>)
        (var type_hi u64)) -> <named RgRange [u64]>
    (block
      (var flags = (call rg_null_value_flags a b))
      (if (&& (call rg_has_bounds a) (call rg_has_bounds b)
               (call rg_may_value a) (call rg_may_value b)
               (! (call rg_u64_mul_overflow (field a Hi) (field b Hi) type_hi)))
        (block
          (= flags (| flags 1))
          (return (call rg_range (* (field a Lo) (field b Lo))
                                 (* (field a Hi) (field b Hi)) flags))))
      (return (call rg_range (: 0 u64) (: 0 u64) flags))))

  (fun rg_f64_down ((var value f64)) -> f64
    (block
      (if (!= value value) (block (return value)))
      (var bits = (bitcast value u64))
      (var sign = (<< (: 1 u64) (: 63 u64)))
      (var pos_inf = (<< (: 2047 u64) (: 52 u64)))
      (var neg_inf = (| sign pos_inf))
      (if (== bits neg_inf) (block (return value)))
      (if (== value 0.0)
        (block (return (bitcast (| sign (: 1 u64)) f64))))
      (if (> value 0.0)
        (block (return (bitcast (- bits (: 1 u64)) f64))))
      (return (bitcast (+ bits (: 1 u64)) f64))))

  (fun rg_f64_up ((var value f64)) -> f64
    (block
      (if (!= value value) (block (return value)))
      (var bits = (bitcast value u64))
      (var sign = (<< (: 1 u64) (: 63 u64)))
      (var pos_inf = (<< (: 2047 u64) (: 52 u64)))
      (if (== bits pos_inf) (block (return value)))
      (if (== value 0.0)
        (block (return (bitcast (: 1 u64) f64))))
      (if (> value 0.0)
        (block (return (bitcast (+ bits (: 1 u64)) f64))))
      (return (bitcast (- bits (: 1 u64)) f64))))

  (fun rg_f64_binary_flags
       ((var a <named RgRange [f64]>) (var b <named RgRange [f64]>)) -> i64
    (block
      ;; Arithmetic may manufacture NaN (for example 0 * infinity), even if
      ;; neither input's data explicitly contained one.
      (return (| (call rg_null_value_flags a b) 8))))

  (fun rg_f64_add
       ((var a <named RgRange [f64]>) (var b <named RgRange [f64]>))
       -> <named RgRange [f64]>
    (block
      (var flags = (call rg_f64_binary_flags a b))
      (if (&& (call rg_has_bounds a) (call rg_has_bounds b)
               (call rg_may_value a) (call rg_may_value b))
        (block
          (var lo = (+ (field a Lo) (field b Lo)))
          (var hi = (+ (field a Hi) (field b Hi)))
          (if (&& (== lo lo) (== hi hi))
            (block
              (= flags (| flags 1))
              (return (call rg_range (call rg_f64_down lo)
                                     (call rg_f64_up hi) flags))))))
      (return (call rg_range 0.0 0.0 flags))))

  (fun rg_f64_sub
       ((var a <named RgRange [f64]>) (var b <named RgRange [f64]>))
       -> <named RgRange [f64]>
    (block
      (var flags = (call rg_f64_binary_flags a b))
      (if (&& (call rg_has_bounds a) (call rg_has_bounds b)
               (call rg_may_value a) (call rg_may_value b))
        (block
          (var lo = (- (field a Lo) (field b Hi)))
          (var hi = (- (field a Hi) (field b Lo)))
          (if (&& (== lo lo) (== hi hi))
            (block
              (= flags (| flags 1))
              (return (call rg_range (call rg_f64_down lo)
                                     (call rg_f64_up hi) flags))))))
      (return (call rg_range 0.0 0.0 flags))))

  (fun rg_f64_mul
       ((var a <named RgRange [f64]>) (var b <named RgRange [f64]>))
       -> <named RgRange [f64]>
    (block
      (var flags = (call rg_f64_binary_flags a b))
      (if (&& (call rg_has_bounds a) (call rg_has_bounds b)
               (call rg_may_value a) (call rg_may_value b))
        (block
          (var p0 = (* (field a Lo) (field b Lo)))
          (var p1 = (* (field a Lo) (field b Hi)))
          (var p2 = (* (field a Hi) (field b Lo)))
          (var p3 = (* (field a Hi) (field b Hi)))
          (if (&& (== p0 p0) (== p1 p1)
                   (== p2 p2) (== p3 p3))
            (block
              (var min_value = p0)
              (var max_value = p0)
              (if (< p1 min_value) (block (= min_value p1)))
              (if (< p2 min_value) (block (= min_value p2)))
              (if (< p3 min_value) (block (= min_value p3)))
              (if (> p1 max_value) (block (= max_value p1)))
              (if (> p2 max_value) (block (= max_value p2)))
              (if (> p3 max_value) (block (= max_value p3)))
              (= flags (| flags 1))
              (return (call rg_range (call rg_f64_down min_value)
                                     (call rg_f64_up max_value) flags))))))
      (return (call rg_range 0.0 0.0 flags))))

  (fun rg_f64_neg ((var a <named RgRange [f64]>)) -> <named RgRange [f64]>
    (block
      (var flags = (field a Flags))
      (if (call rg_has_bounds a)
        (block (return (call rg_range (- 0.0 (field a Hi))
                                      (- 0.0 (field a Lo)) flags))))
      (return (call rg_range 0.0 0.0 flags))))

  (fun rg_f64_from_i64 ((var a <named RgRange [i64]>)) -> <named RgRange [f64]>
    (block
      (var flags = (& (field a Flags) 7))
      (if (call rg_has_bounds a)
        (block
          (return (call rg_range
            (call rg_f64_down (cast (field a Lo) f64))
            (call rg_f64_up (cast (field a Hi) f64)) flags))))
      (return (call rg_range 0.0 0.0 flags))))

  (fun rg_f64_from_u64 ((var a <named RgRange [u64]>)) -> <named RgRange [f64]>
    (block
      (var flags = (& (field a Flags) 7))
      (if (call rg_has_bounds a)
        (block
          (return (call rg_range
            (call rg_f64_down (cast (field a Lo) f64))
            (call rg_f64_up (cast (field a Hi) f64)) flags))))
      (return (call rg_range 0.0 0.0 flags)))))
