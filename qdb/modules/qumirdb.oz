(block
  (pragma language overloads)

  (type StringView <struct (Data <ptr u8>) (Size i64)>)
  (type OwnedString <struct (Data <ptr u8>) (Size i64)>)
  ;; A one-field wrapper, not a bare i128: keeping BinInt nominally
  ;; distinct stops f64 from implicitly converting into it, which would
  ;; drag the decimal operator overloads into ordinary float arithmetic.
  (type BinInt <struct (V i128)>)
  (type DATE i32)
  ;; Nullable stores a regular value plus a validity bit. Most operators are
  ;; null-propagating: NULL + 1 -> NULL, NULL < 10 -> NULL, 5 < 10 -> TRUE.
  ;; SQL boolean logic is special and is implemented by Nullable[bool] overloads:
  ;; FALSE AND NULL -> FALSE, TRUE OR NULL -> TRUE, NULL AND TRUE -> NULL,
  ;; NULL OR FALSE -> NULL, NOT NULL -> NULL.
  (type Nullable [T] <struct (Value T) (Valid bool)>)

  (type null_tag <struct (Tag i8)>)

  (type TColumn <struct
    (Data <ptr i8>)
    (DataBitOffset i32)
    (Mask <ptr u8>)
    (MaskBitOffset i32)
    (Offsets <ptr i64>)
    (OffsetWidth u8)>)

  (type TRowSet <struct
    (Columns <ptr TColumn>)
    (ColumnCount i64)
    (RowCount i64)
    (Selection <ptr u8>)
    (Hash <ptr u64>)
    (Destroy <ptr i64>)
    (Private <ptr i64>)
    (RefCount i64)>)

  (type HashTable <struct
    (Keys <ptr u8>)
    (Ctrl <ptr u8>)
    ;; Dense-state tables (joins/string reducers): physical slot -> dense ID.
    ;; Physical-state aggregates: dense output ID -> physical slot. Their hot
    ;; update path indexes AggBuffers directly by the probe result.
    (SlotId <ptr i64>)
    (GroupKeys <ptr u8>)
    (AggBuffers <ptr <ptr i64>>)
    (OwnedBlocks <ptr <ptr u8>>)
    (OwnedBlockCount i64)
    (OwnedBlockCapacity i64)
    (Capacity i64)
    (Size i64)
    (NumAggs i64)
    (NumKeys i64)
    (KeySize i64)>)

  (type PairBuffer <struct
    (Count i64)
    (Capacity i64)
    (Data <ptr i64>)>)

  (fun bitoff ((var bitmap <ptr u8>) (var index i64) (var bit_offset i64)) -> bool
    (attrs cacheable)
    (block
      (var bit_index = (+ index bit_offset))
      (var byte_index = (>> bit_index (: 3 i64)))
      (var bit_pos = (& bit_index (: 7 i64)))
      (var byte = (cast (index bitmap byte_index) i64))
      (return (!= (& (>> byte bit_pos) (: 1 i64)) (: 0 i64)))))

  (fun qdb_alloc ((var size i64)) -> <ptr i8> (attrs extern) (block))
  (fun qdb_free ((var ptr <ptr i8>)) -> void (attrs extern) (block))

  ;; realloc implemented in Oz on top of qdb_alloc/qdb_free: the caller passes
  ;; the number of live bytes to preserve (old_size), so we copy only that much
  ;; instead of the whole old allocation. Word-copy the 8-byte-aligned bulk, then
  ;; a byte tail for any remainder.
  (fun qdb_realloc
       ((var ptr <ptr i8>) (var old_size i64) (var new_size i64)) -> <ptr i8>
    (block
      (if (== (cast ptr i64) (: 0 i64))
        (block (return (call qdb_alloc new_size))))
      (var new_ptr = (call qdb_alloc new_size))
      (if (== (cast new_ptr i64) (: 0 i64))
        (block (return (cast (: 0 i64) <ptr i8>))))
      (var copy = old_size)
      (if (< new_size copy) (block (= copy new_size)))
      (var words = (>> copy (: 3 i64)))
      (var src_words = (cast ptr <ptr i64>))
      (var dst_words = (cast new_ptr <ptr i64>))
      (var i = (: 0 i64))
      (while (< i words)
        (block
          (= dst_words [i] (index src_words i))
          (= i (+ i (: 1 i64)))))
      (var b = (<< words (: 3 i64)))
      (while (< b copy)
        (block
          (= new_ptr [b] (index ptr b))
          (= b (+ b (: 1 i64)))))
      (call qdb_free ptr)
      (return new_ptr)))

  ;; 128-bit multiply (compiler-rt __multi3). LLVM emits calls to this once the
  ;; O3 autovectorizer widens decimal arithmetic; wasm has no native i128, so
  ;; defining it here lets wasm-ld resolve it inside the module instead of
  ;; importing a JS shim (which would cross the wasm->JS boundary per multiply).
  ;; sret ABI: writes the low 128 bits of (a_hi:a_lo)*(b_hi:b_lo) to ret[0..1].
  ;; Computed with 32-bit limbs (u64 shifts are logical) — no i128 needed.
  (fun __multi3
       ((var ret <ptr i64>) (var a_lo i64) (var a_hi i64)
        (var b_lo i64) (var b_hi i64)) -> void (attrs used)
    (block
      (var mask = (: 4294967295 u64))
      (var al = (cast a_lo u64))
      (var bl = (cast b_lo u64))
      (var a0 = (& al mask))
      (var a1 = (>> al (: 32 u64)))
      (var b0 = (& bl mask))
      (var b1 = (>> bl (: 32 u64)))
      (var t00 = (* a0 b0))
      (var t01 = (* a0 b1))
      (var t10 = (* a1 b0))
      (var t11 = (* a1 b1))
      (var mid = (+ (+ (>> t00 (: 32 u64)) (& t01 mask)) (& t10 mask)))
      (var lo = (+ (& t00 mask) (<< (& mid mask) (: 32 u64))))
      (var hi = (+ (+ (+ t11 (>> t01 (: 32 u64))) (>> t10 (: 32 u64)))
                   (>> mid (: 32 u64))))
      (= hi (+ hi (+ (* al (cast b_hi u64)) (* (cast a_hi u64) bl))))
      (= ret [(: 0 i64)] (cast lo i64))
      (= ret [(: 1 i64)] (cast hi i64))))

  (fun qdb_filter_string_compare
       ((var left <ptr u8>)
        (var left_size i64)
        (var right <ptr u8>)
        (var right_size i64)) -> i64
       (attrs extern) (block))

  (fun qdb_string_hash_bytes ((var data <ptr u8>) (var size i64)) -> i64
       (attrs extern) (block))

  (fun qdb_string_view_sql_like
       ((var str StringView) (var pattern StringView)) -> i64
       (attrs extern) (block))

  (fun qdb_like_equals ((var str StringView) (var lit StringView)) -> i64
       (attrs extern) (block))
  (fun qdb_like_prefix ((var str StringView) (var lit StringView)) -> i64
       (attrs extern) (block))
  (fun qdb_like_suffix ((var str StringView) (var lit StringView)) -> i64
       (attrs extern) (block))
  ;; Tests 16 start positions at once: a start survives only if both the first
  ;; and the last literal byte match; memcmp confirms the survivors.
  (fun qdb_like_probe16
       ((var s i64) (var p <ptr u8>) (var m i64) (var first u8) (var last u8)) -> bool
    (attrs inline)
    (block
      (var head = (cast s <ptr <vec u8 16>>))
      (var tail = (cast (+ s (- m 1)) <ptr <vec u8 16>>))
      (var hit = (& (== (index head 0) first) (== (index tail 0) last)))
      (if (! (call builtin::any hit)) (block (return #f)))
      (var mask = (cast (call builtin::movemask hit) u64))
      (while (!= mask (: 0 u64))
        (block
          (if (== (call builtin::memcmp
                    (cast (+ s (call builtin::cttz mask)) <ptr u8>) p m) (: 0 i32))
            (block (return #t)))
          (= mask (& mask (- mask (: 1 u64))))))
      (return #f)))

  (fun qdb_like_contains ((var str StringView) (var lit StringView)) -> i64
    (block
      (var m = (field lit Size))
      (var n = (field str Size))
      (if (== m 0) (block (return 1)))
      (if (< n m) (block (return 0)))
      (var s = (cast (field str Data) i64))
      (var p = (field lit Data))
      (var first = (index p 0))
      (var last = (index p (- m 1)))
      ;; A block of 16 starts reads m + 15 bytes.
      (var tail = (- n (+ m 15)))
      (var pos = 0)
      (if (>= tail 0)
        (block
          (while (< pos tail)
            (block
              (if (call qdb_like_probe16 (+ s pos) p m first last)
                (block (return 1)))
              (= pos (+ pos 16))))
          ;; The last block overlaps the previous one instead of overreading.
          (if (call qdb_like_probe16 (+ s tail) p m first last)
            (block (return 1)))
          (return 0)))
      (var data = (field str Data))
      (while (<= pos (- n m))
        (block
          (if (&& (== (index data pos) first)
                  (== (index data (+ pos (- m 1))) last))
            (block
              (if (== (call builtin::memcmp (cast (+ s pos) <ptr u8>) p m) (: 0 i32))
                (block (return 1)))))
          (= pos (+ pos 1))))
      (return 0)))

  (fun qdb_sv_sv_eq ((var left StringView) (var right StringView)) -> bool (attrs (operator "=="))
    (block (return (== (call qdb_filter_string_compare (field left Data) (field left Size) (field right Data) (field right Size)) (: 0 i64)))))
  (fun qdb_sv_sv_ne ((var left StringView) (var right StringView)) -> bool (attrs (operator "!="))
    (block (return (!= (call qdb_filter_string_compare (field left Data) (field left Size) (field right Data) (field right Size)) (: 0 i64)))))
  (fun qdb_sv_sv_lt ((var left StringView) (var right StringView)) -> bool (attrs (operator "<"))
    (block (return (< (call qdb_filter_string_compare (field left Data) (field left Size) (field right Data) (field right Size)) (: 0 i64)))))
  (fun qdb_sv_sv_le ((var left StringView) (var right StringView)) -> bool (attrs (operator "<="))
    (block (return (<= (call qdb_filter_string_compare (field left Data) (field left Size) (field right Data) (field right Size)) (: 0 i64)))))
  (fun qdb_sv_sv_gt ((var left StringView) (var right StringView)) -> bool (attrs (operator ">"))
    (block (return (> (call qdb_filter_string_compare (field left Data) (field left Size) (field right Data) (field right Size)) (: 0 i64)))))
  (fun qdb_sv_sv_ge ((var left StringView) (var right StringView)) -> bool (attrs (operator ">="))
    (block (return (>= (call qdb_filter_string_compare (field left Data) (field left Size) (field right Data) (field right Size)) (: 0 i64)))))

  (fun strlen ((var str StringView)) -> i64
    (block (return (field str Size))))

  ;; String literals are emitted as StringView (see the SQL parser), so the literal
  ;; comparison overloads are gone — qdb_sv_sv_* above handles every comparison.

  ;; DATE columns are erased to plain i32 at the schema boundary (parquet DATE32 ->
  ;; TIntegerType::I32), so `date_col OP 'YYYY-MM-DD'` reaches here as (i32, StringView).
  ;; SQL implicitly casts the date string to a date, so parse the literal via qdb_sql_date
  ;; and compare as i32. Only fires for i32-vs-StringView, never for StringView pairs.
  (fun qdb_i32_svdate_eq ((var l i32) (var r StringView)) -> bool (attrs (operator "=="))
    (block (return (== l (call qdb_date_to_i32 (call qdb_sql_date r))))))
  (fun qdb_i32_svdate_ne ((var l i32) (var r StringView)) -> bool (attrs (operator "!="))
    (block (return (!= l (call qdb_date_to_i32 (call qdb_sql_date r))))))
  (fun qdb_i32_svdate_lt ((var l i32) (var r StringView)) -> bool (attrs (operator "<"))
    (block (return (< l (call qdb_date_to_i32 (call qdb_sql_date r))))))
  (fun qdb_i32_svdate_le ((var l i32) (var r StringView)) -> bool (attrs (operator "<="))
    (block (return (<= l (call qdb_date_to_i32 (call qdb_sql_date r))))))
  (fun qdb_i32_svdate_gt ((var l i32) (var r StringView)) -> bool (attrs (operator ">"))
    (block (return (> l (call qdb_date_to_i32 (call qdb_sql_date r))))))
  (fun qdb_i32_svdate_ge ((var l i32) (var r StringView)) -> bool (attrs (operator ">="))
    (block (return (>= l (call qdb_date_to_i32 (call qdb_sql_date r))))))

  (fun qdb_svdate_i32_eq ((var l StringView) (var r i32)) -> bool (attrs (operator "=="))
    (block (return (== (call qdb_date_to_i32 (call qdb_sql_date l)) r))))
  (fun qdb_svdate_i32_ne ((var l StringView) (var r i32)) -> bool (attrs (operator "!="))
    (block (return (!= (call qdb_date_to_i32 (call qdb_sql_date l)) r))))
  (fun qdb_svdate_i32_lt ((var l StringView) (var r i32)) -> bool (attrs (operator "<"))
    (block (return (< (call qdb_date_to_i32 (call qdb_sql_date l)) r))))
  (fun qdb_svdate_i32_le ((var l StringView) (var r i32)) -> bool (attrs (operator "<="))
    (block (return (<= (call qdb_date_to_i32 (call qdb_sql_date l)) r))))
  (fun qdb_svdate_i32_gt ((var l StringView) (var r i32)) -> bool (attrs (operator ">"))
    (block (return (> (call qdb_date_to_i32 (call qdb_sql_date l)) r))))
  (fun qdb_svdate_i32_ge ((var l StringView) (var r i32)) -> bool (attrs (operator ">="))
    (block (return (>= (call qdb_date_to_i32 (call qdb_sql_date l)) r))))

  ;; Cast a string literal (char*) to a StringView without materializing a managed
  ;; string. TODO: replace with native qumir support for StringView literals.
  (fun qdb_lit_to_sv ((var lit string)) -> StringView (attrs extern (operator "cast")) (block))

  ;; Kernels build literal views from the bytes and a length known at codegen.
  (fun qdb_sv_from_ptr ((var data <ptr u8>) (var size i64)) -> StringView
    (attrs inline)
    (block (return (cast (struct ((Data data) (Size size))) StringView))))

  (fun qdb_substring ((var str StringView) (var start i32) (var length i32)) -> StringView
       (attrs extern) (block))

  ;; SQL `l || r` string concatenation. arena is an opaque TStringArena* the executor
  ;; passes into the kernel; ExpandNullable inserts it as the first argument.
  (fun qdb_string_concat ((var arena <ptr i8>) (var l StringView) (var r StringView)) -> StringView
       (attrs extern) (block))

  ;; REGEXP_REPLACE is typed here under its SQL name, then kernel lowering
  ;; replaces it with qdb_regexp_replace(arena, compiled_regex, str).
  (fun regexp_replace
       ((var str StringView) (var pattern StringView) (var replacement StringView)) -> StringView
       (attrs extern) (block))
  (fun qdb_regexp_replace
       ((var arena <ptr i8>) (var regex <ptr i8>) (var str StringView)) -> StringView
       (attrs extern) (block))

  (fun substr ((var str StringView) (var start i32) (var length i32)) -> StringView
       (attrs (extern qdb_substring)) (block))

  (fun substr ((var str StringView) (var start i64) (var length i64)) -> StringView
      (block (call qdb_substring str (cast start i32) (cast length i32))))

  ;; SQL ROUND(x[, d]) — round half away from zero to d decimals (std::round in the
  ;; runtime). The digit count comes in as an i64 literal from the parser.
  (fun qdb_round ((var value f64) (var digits i32)) -> f64 (attrs extern) (block))
  (fun round ((var value f64) (var digits i64)) -> f64
      (block (call qdb_round value (cast digits i32))))
  (fun round ((var value f64)) -> f64
      (block (call qdb_round value (: 0 i32))))
  (fun qdb_sqrt ((var value f64)) -> f64 (attrs (extern sqrt)) (block))
  ;; Qumir's `%` takes integers only. SQL `%` on DOUBLE is C fmod, as in DuckDB: the
  ;; result takes the dividend's sign, and x % 0.0 is NaN.
  (fun qdb_fmod ((var left f64) (var right f64)) -> f64
    (attrs (extern fmod) (operator "%")) (block))

  ;; Decimal kernel storage. qdb logical Decimal(p,s) is erased to this signed
  ;; 128-bit two's-complement value before qumir compilation; precision/scale
  ;; decisions stay in qdb typing/expansion.
  (fun qdb_pow10_i128 ((var scale i64)) -> BinInt
    (block
      (if (< scale 0)
        (block (return (cast (struct ((V (: 0 i128)))) BinInt))))
      (var acc = (: 1 i128))
      (var base = (: 10 i128))
      (var n = scale)
      (while (> n 0)
        (block
          (if (!= (& n 1) 0)
            (block (= acc (* acc base))))
          (= n (>> n 1))
          (= base (* base base))))
      (return (cast (struct ((V acc))) BinInt))))

  (fun qdb_decimal_from_i64 ((var value i64) (var scale i64)) -> BinInt
    (block
      (var factor BinInt)
      (= factor (call qdb_pow10_i128 scale))
      (return (cast (struct ((V (* (cast value i128) (field factor V))))) BinInt))))
  ;; Matches roundl(value * 10^scale) in the former C++ helper: half away from
  ;; zero, since casting to an integer truncates toward zero.
  (fun qdb_decimal_from_f64 ((var value f64) (var scale i64)) -> BinInt
    (block
      (var factor BinInt)
      (= factor (call qdb_pow10_i128 scale))
      (var scaled f64)
      (= scaled (* value (cast (field factor V) f64)))
      (var rounded i128)
      (if (< scaled 0.0)
        (block (= rounded (cast (- scaled 0.5) i128)))
        (block (= rounded (cast (+ scaled 0.5) i128))))
      (return (cast (struct ((V rounded))) BinInt))))
  ;; value / 10^scale as the nearest double, ties to even. When the scaled value is
  ;; below 2^53 and scale <= 22, both operands are exact doubles and the single
  ;; division is correctly rounded. Otherwise the quotient is computed in integers:
  ;; |value| // 10^scale gets fraction bits by long division until it has at least
  ;; 55 bits (k bits appended), then it is rounded to 53 bits using the dropped bits
  ;; and the remainder as guard and sticky, and scaled by a power of two, which is exact.
  (fun qdb_decimal_to_f64 ((var value BinInt) (var scale i64)) -> f64
    (block
      (var factor BinInt)
      (= factor (call qdb_pow10_i128 scale))
      (var p i128)
      (= p (field factor V))
      (var v i128)
      (= v (field value V))
      (var negative bool)
      (= negative (< v (: 0 i128)))
      (var mag i128)
      (= mag v)
      (if negative (block (= mag (- (: 0 i128) v))))
      (var result f64)
      (= result 0.0)
      (if (&& (< mag (: 9007199254740992 i128)) (<= scale 22))
        (block (= result (/ (cast mag f64) (cast p f64))))
        (if (!= mag (: 0 i128))
          (block
            (var q i128)
            (= q (// mag p))
            (var r i128)
            (= r (- mag (* q p)))
            (var e i64)
            (= e 0)
            ;; r < p <= 10^38, so 2r may overflow i128: compare r with p - r instead.
            (while (< q (: 18014398509481984 i128))
              (block
                (= q (* q (: 2 i128)))
                (if (>= r (- p r))
                  (block
                    (= q (+ q (: 1 i128)))
                    (= r (- r (- p r))))
                  (block (= r (+ r r))))
                (= e (- e 1))))
            (var guard i64)
            (= guard 0)
            (var low i64)
            (= low 0)
            (if (!= r (: 0 i128)) (block (= low 1)))
            (while (>= q (: 9007199254740992 i128))
              (block
                (if (!= guard 0) (block (= low 1)))
                (= guard 0)
                (if (!= (& q (: 1 i128)) (: 0 i128)) (block (= guard 1)))
                (= q (// q (: 2 i128)))
                (= e (+ e 1))))
            (if (!= guard 0)
              (block
                (if (|| (!= low 0) (!= (& q (: 1 i128)) (: 0 i128)))
                  (block (= q (+ q (: 1 i128)))))))
            (= result (cast q f64))
            (while (> e 0)
              (block (= result (* result 2.0)) (= e (- e 1))))
            (while (< e 0)
              (block (= result (* result 0.5)) (= e (+ e 1)))))))
      (if negative (block (= result (- 0.0 result))))
      (return result)))
  (fun qdb_decimal_scale_up ((var value BinInt) (var delta i64)) -> BinInt
    (block
      (var factor BinInt)
      (= factor (call qdb_pow10_i128 delta))
      (return (cast (struct ((V (* (field value V) (field factor V))))) BinInt))))

  (fun qdb_decimal_add ((var left BinInt) (var right BinInt)) -> BinInt
    (attrs (operator "+"))
    (block (return (cast (struct ((V (+ (field left V) (field right V))))) BinInt))))
  (fun qdb_decimal_sub ((var left BinInt) (var right BinInt)) -> BinInt
    (attrs (operator "-"))
    (block (return (cast (struct ((V (- (field left V) (field right V))))) BinInt))))
  ;; The former C++ helper threw on negating I128_MIN; native negation wraps,
  ;; matching how add/sub/mul already behave here.
  (fun qdb_decimal_neg ((var value BinInt)) -> BinInt
    (attrs (operator "-"))
    (block (return (cast (struct ((V (- (field value V))))) BinInt))))
  (fun qdb_abs_i32 ((var value i32)) -> i32
    (attrs extern) (block))
  (fun qdb_abs_i64 ((var value i64)) -> i64
    (attrs extern) (block))
  (fun qdb_fabs ((var value f64)) -> f64
    (attrs extern) (block))
  ;; TODO(qumir): `abs [T]` should work as `zero = cast(0, T); zero - value`,
  ;; but generic arithmetic is still rejected in real kernels. Keep concrete
  ;; overloads until generic numeric constraints/body checking are fixed.
  (fun abs ((var value i32)) -> i32
    (block (return (call qdb_abs_i32 value))))
  (fun abs ((var value i64)) -> i64
    (block (return (call qdb_abs_i64 value))))
  (fun abs ((var value f64)) -> f64
    (block (return (call qdb_fabs value))))
  (fun abs ((var value BinInt)) -> BinInt
    (block
      (var raw i128)
      (= raw (field value V))
      (if (< raw (: 0 i128))
        (block (return (cast (struct ((V (- raw)))) BinInt))))
      (return value)))
  (fun qdb_decimal_mul_i64 ((var left BinInt) (var right i64)) -> BinInt
    (attrs (operator "*"))
    (block (return (cast (struct ((V (* (field left V) (cast right i128))))) BinInt))))
  (fun qdb_decimal_i64_mul ((var left i64) (var right BinInt)) -> BinInt
    (attrs (operator "*"))
    (block (return (cast (struct ((V (* (cast left i128) (field right V))))) BinInt))))
  (fun qdb_decimal_div_i64 ((var left BinInt) (var right i64)) -> BinInt
    (attrs (operator "/"))
    (block (return (cast (struct ((V (// (field left V) (cast right i128))))) BinInt))))
  (fun qdb_decimal_div ((var left BinInt) (var right BinInt)) -> BinInt
    (attrs (operator "/"))
    (block (return (cast (struct ((V (// (field left V) (field right V))))) BinInt))))

  ;; Operands share a scale, so the remainder of the scaled values is the remainder
  ;; at that scale; like SQL `%` it takes the dividend's sign.
  (fun qdb_decimal_rem ((var left BinInt) (var right BinInt)) -> BinInt
    (attrs (operator "%"))
    (block (return (cast (struct ((V (% (field left V) (field right V))))) BinInt))))

  (fun qdb_decimal_eq ((var left BinInt) (var right BinInt)) -> bool
    (attrs (operator "==")) (block (return (== (field left V) (field right V)))))
  (fun qdb_decimal_ne ((var left BinInt) (var right BinInt)) -> bool
    (attrs (operator "!=")) (block (return (!= (field left V) (field right V)))))
  (fun qdb_decimal_lt ((var left BinInt) (var right BinInt)) -> bool
    (attrs (operator "<")) (block (return (< (field left V) (field right V)))))
  (fun qdb_decimal_le ((var left BinInt) (var right BinInt)) -> bool
    (attrs (operator "<=")) (block (return (<= (field left V) (field right V)))))
  (fun qdb_decimal_gt ((var left BinInt) (var right BinInt)) -> bool
    (attrs (operator ">")) (block (return (> (field left V) (field right V)))))
  (fun qdb_decimal_ge ((var left BinInt) (var right BinInt)) -> bool
    (attrs (operator ">=")) (block (return (>= (field left V) (field right V)))))

  (fun qdb_bitmap_set_valid ((var bitmap <ptr u8>) (var index i64) (var valid bool)) -> void
    (block
      (var byte_index = (>> index (: 3 i64)))
      (var bit = (<< (: 1 i64) (& index (: 7 i64))))
      (var byte = (cast (index bitmap byte_index) i64))
      (if valid
        (block
          (= bitmap [byte_index] (cast (| byte bit) u8)))
        (block
          (= bitmap [byte_index] (cast (& byte (^ bit (: 255 i64))) u8))))))


  (fun qdb_date_year ((var days i32)) -> i32 (attrs extern) (block))
  (fun qdb_sql_date ((var date StringView)) -> DATE (attrs extern (operator "cast")) (block))
  (fun qdb_sql_interval ((var amount StringView) (var unit StringView)) -> i32 (attrs extern) (block))

  (fun qdb_date_to_i32 ((var d DATE)) -> i32 (attrs (operator "cast"))
    (block (return (bitcast d i32))))
  (fun qdb_i32_to_date ((var n i32)) -> DATE (attrs (operator "cast"))
    (block (return (bitcast n DATE))))
  ;; DATE is nominal in the frontend; compare its day counts explicitly.
  ;; The i32 <-> DATE casts also cover comparisons with parquet DATE32 columns.
  (fun qdb_date_eq ((var l DATE) (var r DATE)) -> bool (attrs (operator "=="))
    (block (return (== (bitcast l i32) (bitcast r i32)))))
  (fun qdb_date_ne ((var l DATE) (var r DATE)) -> bool (attrs (operator "!="))
    (block (return (!= (bitcast l i32) (bitcast r i32)))))
  (fun qdb_date_lt ((var l DATE) (var r DATE)) -> bool (attrs (operator "<"))
    (block (return (< (bitcast l i32) (bitcast r i32)))))
  (fun qdb_date_le ((var l DATE) (var r DATE)) -> bool (attrs (operator "<="))
    (block (return (<= (bitcast l i32) (bitcast r i32)))))
  (fun qdb_date_gt ((var l DATE) (var r DATE)) -> bool (attrs (operator ">"))
    (block (return (> (bitcast l i32) (bitcast r i32)))))
  (fun qdb_date_ge ((var l DATE) (var r DATE)) -> bool (attrs (operator ">="))
    (block (return (>= (bitcast l i32) (bitcast r i32)))))
  (fun qdb_date_add ((var d DATE) (var n i32)) -> DATE (attrs (operator "+"))
    (block (return (bitcast (+ (bitcast d i32) n) DATE))))
  (fun qdb_date_sub ((var d DATE) (var n i32)) -> DATE (attrs (operator "-"))
    (block (return (bitcast (- (bitcast d i32) n) DATE))))

  ;; nullables
  (fun nullable_from_value [T] ((var value T)) -> <named Nullable [T]> (attrs (operator "cast"))
    (block
      (return (cast (struct ((Value value) (Valid #t))) <named Nullable [T]>))))

  (fun qdb_sql_null () -> null_tag
    (block
      (return (cast (struct ((Tag (: 0 i8)))) null_tag))))

  (fun nullable_from_null [T] ((var value null_tag)) -> <named Nullable [T]> (attrs (operator "cast"))
    (block
      (return (cast (struct ((Valid #f))) <named Nullable [T]>))))

  (fun qdb_is_null [T] ((var x <named Nullable [T]>)) -> bool
    (block
      (return (! (field x Valid)))))

  ;; IS NULL on a non-nullable value is always false.
  (fun qdb_is_null [T] ((var x T)) -> bool
    (block (return #f)))

  ;; An untyped NULL (a bare NULL, or a CASE/COALESCE with only NULL values) is
  ;; null_tag. The concrete overload wins over the generic one above.
  (fun qdb_is_null ((var x null_tag)) -> bool
    (block (return #t)))

  ;; SQL WHERE truthiness: a row passes iff the predicate is TRUE (NULL is not TRUE).
  (fun qdb_is_true ((var x bool)) -> bool
    (block (return x)))
  (fun qdb_is_true ((var x <named Nullable [bool]>)) -> bool
    (block (return (&& (field x Valid) (field x Value)))))
  ;; LIKE and other predicates return i64 (0/1); nonzero is TRUE.
  (fun qdb_is_true ((var x i64)) -> bool
    (block (return (!= x (: 0 i64)))))
  ;; LIKE and other i64-returning predicates, wrapped nullable by the null-propagation
  ;; pass: TRUE iff valid and nonzero.
  (fun qdb_is_true ((var x <named Nullable [i64]>)) -> bool
    (block (return (&& (field x Valid) (!= (field x Value) (: 0 i64))))))


)
