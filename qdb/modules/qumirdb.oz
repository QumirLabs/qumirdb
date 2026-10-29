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
    (KeySize i64)
    ;; The row layout of an aggregate (kernel/aggregation/aggregation_rows.oz):
    ;; one arena row per group, holding its key and states, indexed by salted
    ;; entries. The table is in this layout once RowTable has entries; the
    ;; swiss arrays above are then null. Joins do not use it.
    (RowArena RowArena)
    (RowTable RowTable)
    (RowScratch RowScratch)
    ;; Groups at which a swiss table that must grow moves to the row layout
    ;; instead; 0 keeps it swiss.
    (RowSwitchGroups i64)
    ;; Groups the row entries are sized for at that move, when more than
    ;; twice the groups moved.
    (RowExpectedGroups i64)
    ;; For instrumentation: the builds of the row entries after the table's
    ;; start, the move from the swiss table and each growth, and the swiss
    ;; slots the table moved from (0 when it started in the row layout).
    (RowRebuilds i64)
    (RowMovedFromSlots i64)>)

  (type PairBuffer <struct
    (Count i64)
    (Capacity i64)
    (Data <ptr i64>)>)

  ;; Row table (kernel/row_table.oz): rows in fixed-size blocks.
  (type RowArena <struct
    (Blocks <ptr <ptr u8>>)
    (BlockCount i64)
    (BlockCapacity i64)
    (BlockRows i64)
    (RowBytes i64)
    (Rows i64)
    (CurRows i64)>)

  ;; Entries hold a 16-bit salt above a 48-bit row pointer; index = hash >> Shift.
  (type RowTable <struct
    (Entries <ptr u64>)
    (Capacity i64)
    (Mask u64)
    (Shift i64)
    (Rows i64)
    (Keys i64)>)

  ;; Per-batch lookup scratch of the row table, one slot per looked-up row.
  (type RowScratch <struct
    (Hashes <ptr u64>)
    (Salts <ptr u64>)
    (Offsets <ptr u64>)
    (Entries <ptr u64>)
    (Rows <ptr i64>)
    (Work <ptr i64>)
    (Keys <ptr u8>)
    (Capacity i64)>)

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

  ;; Releases every string of arena; a join residual calls it after each pair.
  (fun qdb_string_arena_reset ((var arena <ptr i8>)) -> void (attrs extern) (block))
  ;; Bytes for a string that a kernel function produces, from the same arena.
  (fun qdb_string_arena_alloc ((var arena <ptr i8>) (var size i64)) -> <ptr u8>
       (attrs extern) (block))

  ;; CAST(<integer> AS VARCHAR): the decimal digits of a magnitude with an optional
  ;; minus sign, in the arena like the result of `||`. The cast rule
  ;; (qdb/kernel/cast_rules.cpp) widens a signed integer to i64 and an unsigned one to
  ;; u64, so a UBIGINT above 2^63-1 keeps its value.
  (fun qdb_string_from_magnitude ((var arena <ptr i8>) (var mag u64) (var negative bool)) -> StringView
    (block
      (var digits = 1)
      (var rest = (// mag (: 10 u64)))
      (while (!= rest (: 0 u64))
        (block
          (= digits (+ digits 1))
          (= rest (// rest (: 10 u64)))))
      (var size = digits)
      (if negative (block (= size (+ size 1))))
      (var out = (call qdb_string_arena_alloc arena size))
      (var pos = size)
      (var i = 0)
      (while (< i digits)
        (block
          (= pos (- pos 1))
          (= out [pos] (cast (+ (% mag (: 10 u64)) (: 48 u64)) u8))
          (= mag (// mag (: 10 u64)))
          (= i (+ i 1))))
      (if negative (block (= out [0] (: 45 u8))))
      (return (cast (struct ((Data out) (Size size))) StringView))))

  (fun qdb_string_from_i64 ((var arena <ptr i8>) (var value i64)) -> StringView
    (block
      ;; The magnitude as u64: negating the most negative i64 fits there.
      (var negative = (< value 0))
      (var mag = (cast value u64))
      (if negative (block (= mag (- (: 0 u64) mag))))
      (return (call qdb_string_from_magnitude arena mag negative))))

  (fun qdb_string_from_u64 ((var arena <ptr i8>) (var value u64)) -> StringView
    (block
      (return (call qdb_string_from_magnitude arena value #f))))

  ;; CAST(<date> AS VARCHAR): the ISO 8601 date yyyy-mm-dd of a day count since
  ;; 1970-01-01 in the proleptic Gregorian calendar, in the arena like `||`. The year,
  ;; month and day come from Howard Hinnant's civil_from_days ("chrono-Compatible
  ;; Low-Level Date Algorithms"): the count is shifted to start at 0000-03-01, so a
  ;; year begins in March and a leap day is its last day, and is split into eras of
  ;; 400 years (146097 days) in which the leap-year pattern repeats. The year has at
  ;; least four digits, more when it needs them, and a minus sign when negative.
  (fun qdb_string_from_date ((var arena <ptr i8>) (var d DATE)) -> StringView
    (block
      (var z = (+ (cast (bitcast d i32) i64) 719468))
      (var era = (// z 146097))
      (if (< z 0) (block (= era (// (- z 146096) 146097))))
      (var doe = (- z (* era 146097)))
      (var yoe = (// (- (+ doe (// doe 36524)) (+ (// doe 1460) (// doe 146096))) 365))
      (var year = (+ yoe (* era 400)))
      (var doy = (- doe (- (+ (* 365 yoe) (// yoe 4)) (// yoe 100))))
      (var mp = (// (+ (* 5 doy) 2) 153))
      (var day = (+ (- doy (// (+ (* 153 mp) 2) 5)) 1))
      (var month = (- mp 9))
      (if (< mp 10) (block (= month (+ mp 3))))
      (if (<= month 2) (block (= year (+ year 1))))
      (var negative = (< year 0))
      (var mag = year)
      (if negative (block (= mag (- 0 year))))
      (var digits = 4)
      (var rest = (// mag 10000))
      (while (!= rest 0)
        (block
          (= digits (+ digits 1))
          (= rest (// rest 10))))
      (var size = (+ digits 6))
      (if negative (block (= size (+ size 1))))
      (var out = (call qdb_string_arena_alloc arena size))
      (var pos = 0)
      (if negative
        (block
          (= out [0] (: 45 u8))
          (= pos 1)))
      (var i = (+ pos digits))
      (while (> i pos)
        (block
          (= i (- i 1))
          (= out [i] (cast (+ (% mag 10) 48) u8))
          (= mag (// mag 10))))
      (= pos (+ pos digits))
      (= out [pos] (: 45 u8))
      (= out [(+ pos 1)] (cast (+ (// month 10) 48) u8))
      (= out [(+ pos 2)] (cast (+ (% month 10) 48) u8))
      (= out [(+ pos 3)] (: 45 u8))
      (= out [(+ pos 4)] (cast (+ (// day 10) 48) u8))
      (= out [(+ pos 5)] (cast (+ (% day 10) 48) u8))
      (return (cast (struct ((Data out) (Size size))) StringView))))

  ;; CAST(<boolean> AS VARCHAR) spells the value like DuckDB and PostgreSQL output.
  ;; CAST(<boolean> AS INTEGER / BIGINT / DOUBLE) is 1 or 0. Without these conversions
  ;; Qumir lowers the cast as a sign extension of the i1, so TRUE would become -1.
  (fun qdb_bool_to_i32 ((var value bool)) -> i32 (attrs (operator "cast"))
    (block
      (if value (block (return (: 1 i32))))
      (return (: 0 i32))))

  (fun qdb_bool_to_i64 ((var value bool)) -> i64 (attrs (operator "cast"))
    (block
      (if value (block (return 1)))
      (return 0)))

  (fun qdb_bool_to_f64 ((var value bool)) -> f64 (attrs (operator "cast"))
    (block
      (if value (block (return 1.0)))
      (return 0.0)))

  (fun qdb_string_from_bool ((var value bool)) -> StringView (attrs (operator "cast"))
    (block
      (if value (block (return (cast "true" StringView))))
      (return (cast "false" StringView))))

  ;; SQL UPPER/LOWER map each character by the simple (one code point to one
  ;; code point) Unicode case mapping, as DuckDB does, in Basic Latin, Latin-1
  ;; Supplement, Latin Extended-A, Latin Extended Additional, the Greek alphabet
  ;; U+0386-U+03CE, Cyrillic and Cyrillic Supplement. Every other character is
  ;; kept: Latin Extended-B, the rest of Greek and Coptic, Greek Extended,
  ;; Armenian, Georgian, fullwidth Latin and others. DuckDB maps some of them
  ;; (Latin Extended-B, IPA, U+0345, Greek U+0370-U+0385 and U+03CF-U+03FF,
  ;; Armenian), so there the results differ. ExpandNullable passes the kernel's
  ;; scratch arena as for `||`.
  (fun qdb_string_upper ((var arena <ptr i8>) (var str StringView)) -> StringView
    (block (return (call qdb_string_case arena str #t))))

  (fun qdb_string_lower ((var arena <ptr i8>) (var str StringView)) -> StringView
    (block (return (call qdb_string_case arena str #f))))

  ;; A mapped character keeps its UTF-8 length but for U+00DF sharp s, which
  ;; grows from two bytes to three in upper case, and U+0130, U+0131, U+017F
  ;; and U+1E9E, which shrink. So one pass writes the result into as many bytes
  ;; as the input has; only an upper-cased string with a sharp s is mapped
  ;; again, into a byte more for each of them.
  (fun qdb_string_case ((var arena <ptr i8>) (var str StringView) (var to_upper bool)) -> StringView
    (attrs cacheable)
    (block
      (var n = (field str Size))
      (if (== n 0) (block (return str)))
      (var out = (call qdb_string_arena_alloc arena n))
      (var size = (call qdb_string_case_into str to_upper out #f))
      (if (< size 0)
        (block
          (= out (call qdb_string_arena_alloc arena (+ n (call qdb_count_sharp_s str))))
          (= size (call qdb_string_case_into str to_upper out #t))))
      (return (cast (struct ((Data out) (Size size))) StringView))))

  ;; C3 is never a continuation byte, so every C3 9F is a U+00DF sharp s.
  (fun qdb_count_sharp_s ((var str StringView)) -> i64
    (attrs cacheable)
    (block
      (var data = (field str Data))
      (var count = 0)
      (var i = 1)
      (while (< i (field str Size))
        (block
          (if (&& (== (index data i) (: 159 u8)) (== (index data (- i 1)) (: 195 u8)))
            (block (= count (+ count 1))))
          (= i (+ i 1))))
      (return count)))

  ;; Writes str with its case mapped to out and returns the size written. out
  ;; holds as many bytes as str and, if grow is set, one more for each sharp s;
  ;; without grow a sharp s to upper-case returns -1.
  (fun qdb_string_case_into
       ((var str StringView) (var to_upper bool) (var out <ptr u8>) (var grow bool)) -> i64
    (attrs cacheable)
    (block
      (var data = (field str Data))
      (var n = (field str Size))
      (var src = (cast data i64))
      (var dst = (cast out i64))
      ;; The ASCII letters that change: a-z to upper case, A-Z to lower case.
      (var first = (: 65 u8))
      (if to_upper (block (= first (: 97 u8))))
      (var i = 0)
      (var o = 0)
      ;; A string shorter than a block of 16 bytes that is ASCII is covered by
      ;; two overlapping blocks of 8 or 4 bytes.
      (if (&& (>= n 8) (< n 16))
        (block
          (if (call qdb_case_ascii_block (cast src <ptr <vec u8 8>>) (cast dst <ptr <vec u8 8>>) first)
            (block
              (if (call qdb_case_ascii_block (cast (- (+ src n) 8) <ptr <vec u8 8>>)
                    (cast (- (+ dst n) 8) <ptr <vec u8 8>>) first)
                (block (return n)))
              (= i 8)
              (= o 8)))))
      (if (&& (>= n 4) (< n 8))
        (block
          (if (call qdb_case_ascii_block (cast src <ptr <vec u8 4>>) (cast dst <ptr <vec u8 4>>) first)
            (block
              (if (call qdb_case_ascii_block (cast (- (+ src n) 4) <ptr <vec u8 4>>)
                    (cast (- (+ dst n) 4) <ptr <vec u8 4>>) first)
                (block (return n)))
              (= i 4)
              (= o 4)))))
      (while (< i n)
        (block
          (var end = n)
          (if (<= (+ i 16) n)
            (block
              (var block_src = (cast (+ src i) <ptr <vec u8 16>>))
              (var block_dst = (cast (+ dst o) <ptr <vec u8 16>>))
              (var done = 16)
              (if (! (call qdb_case_ascii_block block_src block_dst first))
                (block (= done (call qdb_case_utf8_block block_src block_dst to_upper 0))))
              (if (> done 0)
                (block
                  (= i (+ i done))
                  (= o (+ o done))
                  continue))
              (= end (+ i 16)))
            (block
              ;; The last block ends at n and so starts back bytes before i and
              ;; o. When it is ASCII, those bytes were written one to one, o - i
              ;; bytes away, and mapping them again writes the same bytes;
              ;; otherwise the block keeps them.
              (var back = (- (+ i 16) n))
              (if (&& (>= i back) (>= o back))
                (block
                  (var block_src = (cast (- (+ src i) back) <ptr <vec u8 16>>))
                  (var block_dst = (cast (- (+ dst o) back) <ptr <vec u8 16>>))
                  (if (|| (call qdb_case_ascii_block block_src block_dst first)
                          (> (call qdb_case_utf8_block block_src block_dst to_upper back) 0))
                    (block (return (+ o (- n i)))))))))
          ;; One character at a time up to end; the last one may cross it.
          (while (< i end)
            (block
              (var byte = (index data i))
              (if (< byte (: 128 u8))
                (block
                  (if (< (- byte first) (: 26 u8)) (block (= byte (^ byte (: 32 u8)))))
                  (= out [o] byte)
                  (= i (+ i 1))
                  (= o (+ o 1))
                  continue))
              (var c = (cast byte i64))
              (var length = (call qdb_case_utf8_length data n i))
              (if (== length 1)
                (block
                  (= out [o] (cast c u8))
                  (= i (+ i 1))
                  (= o (+ o 1))
                  continue))
              (if (== length 2)
                (block (= c (| (<< (& c 31) 6) (& (cast (index data (+ i 1)) i64) 63)))))
              (if (== length 3)
                (block
                  (= c (| (| (<< (& c 15) 12) (<< (& (cast (index data (+ i 1)) i64) 63) 6))
                          (& (cast (index data (+ i 2)) i64) 63)))))
              (var mapped = (call qdb_case_code_point c to_upper))
              ;; Of the two-byte characters only the sharp s maps to three bytes.
              (if (&& (> mapped 2047) (&& (== length 2) (! grow))) (block (return -1)))
              (= o (+ o (call qdb_utf8_put out o mapped)))
              (= i (+ i length))))))
      (return o)))

  ;; Maps a block of N bytes from src to dst if they are all ASCII; otherwise
  ;; returns false and writes nothing. first is the first letter that changes.
  (fun qdb_case_ascii_block [(const N i64)]
       ((var src <ptr <vec u8 N>>) (var dst <ptr <vec u8 N>>) (var first u8)) -> bool
    (attrs inline)
    (block
      (var bytes = (index src 0))
      (if (call builtin::any (>= bytes (: 128 u8))) (block (return #f)))
      (= dst [0] (call builtin::select (< (- bytes first) (: 26 u8)) (^ bytes (: 32 u8)) bytes))
      (return #t)))

  ;; Byte length of the character at data[i] if case mapping may change it: a
  ;; two-byte character with the lead C2-C5 or CE-D4, a three-byte one from
  ;; U+1E00-U+1EFF. Every other byte, a byte of a character that keeps its
  ;; case or one that is not UTF-8, is copied alone: the length is 1.
  (fun qdb_case_utf8_length ((var data <ptr u8>) (var n i64) (var i i64)) -> i64
    (attrs cacheable inline)
    (block
      (var lead = (cast (index data i) i64))
      (if (&& (|| (call qdb_case_in lead 194 197) (call qdb_case_in lead 206 212))
              (call qdb_utf8_tail data n (+ i 1)))
        (block (return 2)))
      (if (&& (== lead 225)
              (&& (call qdb_utf8_tail data n (+ i 2))
                  (call qdb_case_in (cast (index data (+ i 1)) i64) 184 187)))
        (block (return 3)))
      (return 1)))

  (fun qdb_case_in ((var c i64) (var lo i64) (var hi i64)) -> bool
    (attrs cacheable inline)
    (block (return (&& (>= c lo) (<= c hi)))))

  (fun qdb_utf8_tail ((var data <ptr u8>) (var n i64) (var at i64)) -> bool
    (attrs cacheable inline)
    (block (return (&& (< at n) (== (& (cast (index data at) i64) 192) 128)))))

  ;; Writes the code point c, below U+10000, as UTF-8 at out[at]; returns its
  ;; byte length.
  (fun qdb_utf8_put ((var out <ptr u8>) (var at i64) (var c i64)) -> i64
    (attrs cacheable inline)
    (block
      (if (< c 128)
        (block
          (= out [at] (cast c u8))
          (return 1)))
      (if (< c 2048)
        (block
          (= out [at] (cast (| 192 (>> c 6)) u8))
          (= out [(+ at 1)] (cast (| 128 (& c 63)) u8))
          (return 2)))
      (= out [at] (cast (| 224 (>> c 12)) u8))
      (= out [(+ at 1)] (cast (| 128 (& (>> c 6) 63)) u8))
      (= out [(+ at 2)] (cast (| 128 (& c 63)) u8))
      (return 3)))

  ;; Case mapping of one code point below U+10000 by the tables for a block.
  (fun qdb_case_code_point ((var c i64) (var to_upper bool)) -> i64
    (attrs cacheable)
    (block
      (var lanes = (+ (: (vec 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0) <vec u16 16>) (cast c u16)))
      (if to_upper (block (return (cast (index (call qdb_upper_code_points lanes) 0) i64))))
      (return (cast (index (call qdb_lower_code_points lanes) 0) i64))))

  ;; Maps a block of 16 bytes of ASCII and two-byte characters from src to dst,
  ;; keeping the first keep bytes of dst. Returns how many bytes it mapped: 16,
  ;; or 15 when the last byte is a lead byte. Returns 0 and writes nothing if a
  ;; character of the block changes its UTF-8 length or the block has the lead
  ;; byte E1 of U+1000-U+1FFF, which holds Latin Extended Additional.
  (fun qdb_case_utf8_block
       ((var src <ptr <vec u8 16>>) (var dst <ptr <vec u8 16>>) (var to_upper bool) (var keep i64)) -> i64
    (attrs cacheable)
    (block
      (var bytes = (index src 0))
      (if (call builtin::any (== bytes (: 225 u8))) (block (return 0)))
      ;; The neighbours of each byte. The first byte has no previous one in the
      ;; block and the last byte no next one: they see themselves instead, which
      ;; never pairs a lead byte with a continuation byte.
      (var prev = (call builtin::shuffle bytes (vec 0 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14)))
      (var next = (call builtin::shuffle bytes (vec 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 15)))
      ;; lead marks the first byte of a two-byte character, tail its second.
      (var lead = (&& (call qdb_utf8_lead2 bytes) (call qdb_utf8_cont next)))
      (var tail = (&& (call qdb_utf8_cont bytes) (call qdb_utf8_lead2 prev)))
      (var wide = (cast bytes <vec u16 16>))
      ;; The code point of each character at its first byte; zero, which no
      ;; rule changes, at the other bytes.
      (var ascii = (< bytes (: 128 u8)))
      (var code = (call builtin::select lead
                    (| (<< (& wide 31) 6) (& (cast next <vec u16 16>) 63))
                    (call builtin::select ascii wide
                      (: (vec 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0) <vec u16 16>))))
      (var mapped = code)
      (if to_upper
        (block (= mapped (call qdb_upper_code_points code)))
        (block (= mapped (call qdb_lower_code_points code))))
      (if (call builtin::any (&& lead (|| (< mapped 128) (> mapped 2047))))
        (block (return 0)))
      ;; Other bytes, a continuation without its lead or a byte of a longer
      ;; character, stay as they are.
      (var result = (call builtin::select ascii (cast mapped <vec u8 16>) bytes))
      (= result (call builtin::select lead (cast (| (>> mapped 6) 192) <vec u8 16>) result))
      (var second = (cast (| (& mapped 63) 128) <vec u8 16>))
      (= result (call builtin::select tail
                  (call builtin::shuffle second (vec 0 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14)) result))
      (if (> keep 0)
        (block
          (var lane = (: (vec 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15) <vec u8 16>))
          (= result (call builtin::select (< lane (cast keep u8)) (index dst 0) result))))
      (= dst [0] result)
      (if (>= (index bytes 15) (: 192 u8)) (block (return 15)))
      (return 16)))

  ;; Lead bytes C2-DF of two-byte characters.
  (fun qdb_utf8_lead2 ((var bytes <vec u8 16>)) -> <vec bool 16>
    (attrs cacheable inline)
    (block (return (< (- bytes (: 194 u8)) (: 30 u8)))))

  ;; Continuation bytes 80-BF.
  (fun qdb_utf8_cont ((var bytes <vec u8 16>)) -> <vec bool 16>
    (attrs cacheable inline)
    (block (return (== (& bytes (: 192 u8)) (: 128 u8)))))

  ;; The rules of the case mapping tables below. Each takes the result so far
  ;; and the code points c of a block, and replaces the result where the rule
  ;; applies: here with c + delta for c from lo to hi.
  (fun qdb_case_shift
       ((var result <vec u16 16>) (var c <vec u16 16>) (var lo i64) (var hi i64) (var delta i64))
       -> <vec u16 16>
    (attrs cacheable inline)
    (block
      (return (call builtin::select (&& (>= c (cast lo u16)) (<= c (cast hi u16)))
                (+ c (cast delta u16)) result))))

  ;; to for the code point from.
  (fun qdb_case_set
       ((var result <vec u16 16>) (var c <vec u16 16>) (var from i64) (var to i64)) -> <vec u16 16>
    (attrs cacheable inline)
    (block (return (call builtin::select (== c (cast from u16)) (+ c (cast (- to from) u16)) result))))

  ;; A run of case pairs from lo to hi starts with an upper case letter, and
  ;; each lower case letter follows its upper case one: the lower case letters
  ;; are at an odd distance from lo.
  (fun qdb_case_pairs_upper
       ((var result <vec u16 16>) (var c <vec u16 16>) (var lo i64) (var hi i64)) -> <vec u16 16>
    (attrs cacheable inline)
    (block
      (return (call builtin::select
                (&& (&& (>= c (cast lo u16)) (<= c (cast hi u16)))
                    (== (& (- c (cast lo u16)) (: 1 u16)) (: 1 u16)))
                (- c (: 1 u16)) result))))

  (fun qdb_case_pairs_lower
       ((var result <vec u16 16>) (var c <vec u16 16>) (var lo i64) (var hi i64)) -> <vec u16 16>
    (attrs cacheable inline)
    (block
      (return (call builtin::select
                (&& (&& (>= c (cast lo u16)) (<= c (cast hi u16)))
                    (== (& (- c (cast lo u16)) (: 1 u16)) (: 0 u16)))
                (+ c (: 1 u16)) result))))

  ;; Whether a code point of c is from lo to hi.
  (fun qdb_case_any_in ((var c <vec u16 16>) (var lo i64) (var hi i64)) -> bool
    (attrs cacheable inline)
    (block (return (call builtin::any (&& (>= c (cast lo u16)) (<= c (cast hi u16)))))))

  ;; Case mapping of the code points of a block. Each rule replaces the result
  ;; for the code points it covers; a later rule overrides an earlier one. The
  ;; rules are grouped by alphabet, and a group is skipped unless a code point
  ;; of the block is in its range.
  (fun qdb_upper_code_points ((var c <vec u16 16>)) -> <vec u16 16>
    (attrs cacheable)
    (block
      (var r = (call qdb_case_shift c c 97 122 -32))
      ;; Latin-1 Supplement: all but U+00F7 division sign, U+00B5 micro sign,
      ;; U+00DF sharp s (to U+1E9E, as DuckDB), U+00FF y with diaeresis.
      (if (call qdb_case_any_in c 181 255)
        (block
          (= r (call qdb_case_shift r c 224 254 -32))
          (= r (call qdb_case_set r c 247 247))
          (= r (call qdb_case_set r c 181 924))
          (= r (call qdb_case_set r c 223 7838))
          (= r (call qdb_case_set r c 255 376))))
      ;; Latin Extended-A U+0100-U+017F: case pairs, U+0131 dotless i, U+017F long s.
      (if (call qdb_case_any_in c 256 383)
        (block
          (= r (call qdb_case_pairs_upper r c 256 311))
          (= r (call qdb_case_pairs_upper r c 313 328))
          (= r (call qdb_case_pairs_upper r c 330 375))
          (= r (call qdb_case_pairs_upper r c 377 382))
          (= r (call qdb_case_set r c 305 73))
          (= r (call qdb_case_set r c 383 83))))
      ;; Greek alphabet U+0386-U+03CE, with tonos and U+03C2 final sigma.
      (if (call qdb_case_any_in c 940 974)
        (block
          (= r (call qdb_case_shift r c 945 971 -32))
          (= r (call qdb_case_set r c 962 931))
          (= r (call qdb_case_set r c 940 902))
          (= r (call qdb_case_shift r c 941 943 -37))
          (= r (call qdb_case_set r c 972 908))
          (= r (call qdb_case_shift r c 973 974 -63))))
      ;; Cyrillic U+0400-U+045F.
      (if (call qdb_case_any_in c 1072 1119)
        (block
          (= r (call qdb_case_shift r c 1072 1103 -32))
          (= r (call qdb_case_shift r c 1104 1119 -80))))
      ;; Cyrillic U+0460-U+04FF and Cyrillic Supplement: case pairs, U+04CF palochka.
      (if (call qdb_case_any_in c 1120 1327)
        (block
          (= r (call qdb_case_pairs_upper r c 1120 1153))
          (= r (call qdb_case_pairs_upper r c 1162 1215))
          (= r (call qdb_case_pairs_upper r c 1217 1230))
          (= r (call qdb_case_pairs_upper r c 1232 1327))
          (= r (call qdb_case_set r c 1231 1216))))
      ;; Latin Extended Additional U+1E00-U+1EFF: case pairs, U+1E9B long s with dot above.
      (if (call qdb_case_any_in c 7680 7935)
        (block
          (= r (call qdb_case_pairs_upper r c 7680 7829))
          (= r (call qdb_case_pairs_upper r c 7840 7935))
          (= r (call qdb_case_set r c 7835 7776))))
      (return r)))

  (fun qdb_lower_code_points ((var c <vec u16 16>)) -> <vec u16 16>
    (attrs cacheable)
    (block
      (var r = (call qdb_case_shift c c 65 90 32))
      ;; Latin-1 Supplement: all but U+00D7 multiplication sign.
      (if (call qdb_case_any_in c 192 222)
        (block
          (= r (call qdb_case_shift r c 192 222 32))
          (= r (call qdb_case_set r c 215 215))))
      ;; Latin Extended-A U+0100-U+017F: case pairs, U+0130 I with dot above,
      ;; U+0178 Y with diaeresis.
      (if (call qdb_case_any_in c 256 382)
        (block
          (= r (call qdb_case_pairs_lower r c 256 311))
          (= r (call qdb_case_pairs_lower r c 313 328))
          (= r (call qdb_case_pairs_lower r c 330 375))
          (= r (call qdb_case_pairs_lower r c 377 382))
          (= r (call qdb_case_set r c 304 105))
          (= r (call qdb_case_set r c 376 255))))
      ;; Greek alphabet U+0386-U+03CE, with tonos.
      (if (call qdb_case_any_in c 902 939)
        (block
          (= r (call qdb_case_set r c 902 940))
          (= r (call qdb_case_shift r c 904 906 37))
          (= r (call qdb_case_set r c 908 972))
          (= r (call qdb_case_shift r c 910 911 63))
          (= r (call qdb_case_shift r c 913 929 32))
          (= r (call qdb_case_shift r c 931 939 32))))
      ;; Cyrillic U+0400-U+045F.
      (if (call qdb_case_any_in c 1024 1071)
        (block
          (= r (call qdb_case_shift r c 1024 1039 80))
          (= r (call qdb_case_shift r c 1040 1071 32))))
      ;; Cyrillic U+0460-U+04FF and Cyrillic Supplement: case pairs, U+04C0 palochka.
      (if (call qdb_case_any_in c 1120 1327)
        (block
          (= r (call qdb_case_pairs_lower r c 1120 1153))
          (= r (call qdb_case_pairs_lower r c 1162 1215))
          (= r (call qdb_case_pairs_lower r c 1217 1230))
          (= r (call qdb_case_pairs_lower r c 1232 1327))
          (= r (call qdb_case_set r c 1216 1231))))
      ;; Latin Extended Additional U+1E00-U+1EFF: case pairs, U+1E9E capital sharp s.
      (if (call qdb_case_any_in c 7680 7935)
        (block
          (= r (call qdb_case_pairs_lower r c 7680 7829))
          (= r (call qdb_case_pairs_lower r c 7840 7935))
          (= r (call qdb_case_set r c 7838 223))))
      (return r)))

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
  ;; CAST(decimal AS integer): round half away from zero, as DuckDB and PostgreSQL do.
  ;; qdb has no runtime error channel, so a value outside the signed `bits`-wide range
  ;; saturates to its nearest bound, like CAST(double AS integer) does.
  (fun qdb_decimal_to_int_sat ((var value BinInt) (var scale i64) (var bits i64)) -> i64
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
      (var q i128)
      (= q (// mag p))
      (var r i128)
      (= r (- mag (* q p)))
      (if (>= r (- p r)) (block (= q (+ q (: 1 i128)))))
      (var limit i128)
      (= limit (: 1 i128))
      (var n i64)
      (= n (- bits 1))
      (while (> n 0)
        (block (= limit (* limit (: 2 i128))) (= n (- n 1))))
      (if negative
        (block
          (if (> q limit) (block (= q limit)))
          (= q (- (: 0 i128) q)))
        (block
          (if (>= q limit) (block (= q (- limit (: 1 i128)))))))
      (return (cast q i64))))
  (fun qdb_decimal_to_i64 ((var value BinInt) (var scale i64)) -> i64
    (block (return (call qdb_decimal_to_int_sat value scale (: 64 i64)))))
  (fun qdb_decimal_to_i32 ((var value BinInt) (var scale i64)) -> i32
    (block (return (cast (call qdb_decimal_to_int_sat value scale (: 32 i64)) i32))))
  ;; A decimal constant given by its unscaled value; the scale is tracked by typing.
  (fun qdb_decimal_from_unscaled ((var value i64)) -> BinInt
    (block (return (cast (struct ((V (cast value i128)))) BinInt))))
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
  ;; Narrow abs goes through i32: extern i8/i16 parameters carry no signext.
  (fun qdb_abs_i8 ((var value i32)) -> i32
    (attrs extern) (block))
  (fun qdb_abs_i16 ((var value i32)) -> i32
    (attrs extern) (block))
  (fun qdb_abs_i32 ((var value i32)) -> i32
    (attrs extern) (block))
  (fun qdb_abs_i64 ((var value i64)) -> i64
    (attrs extern) (block))
  (fun qdb_fabs ((var value f64)) -> f64
    (attrs extern) (block))
  ;; TODO(qumir): `abs [T]` should work as `zero = cast(0, T); zero - value`,
  ;; but generic arithmetic is still rejected in real kernels. Keep concrete
  ;; overloads until generic numeric constraints/body checking are fixed.
  (fun abs ((var value i8)) -> i8
    (block (return (cast (call qdb_abs_i8 (cast value i32)) i8))))
  (fun abs ((var value i16)) -> i16
    (block (return (cast (call qdb_abs_i16 (cast value i32)) i16))))
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
