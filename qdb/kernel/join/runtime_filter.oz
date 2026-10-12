(block
  ;; Runtime filter kernel: every operation that reads or writes filter
  ;; contents. It has three entry points, at the end of this file:
  ;;   rf_build    adds a batch of hashes to a producer's contribution;
  ;;   rf_publish  merges the contributions into the published filter;
  ;;   rf_probe    tests a batch of hashes against the published filter.
  ;; C++ (qdb/exec/runtime_filter.cpp) provides the storage that the state
  ;; structs below point to, and owns publication and probe policy.
  ;; Conventions:
  ;;   - a null selection selects every row;
  ;;   - probes write a nonzero byte for a kept row and 0 otherwise, and
  ;;     return the number of kept rows;
  ;;   - a Bloom block is eight u64 words with one bit set in each word, and
  ;;     block_mask is (blocks - 1) for a power-of-two block count.
  ;; Needs sort/radix.oz and aggregation/swiss_group.oz in the same program.

  ;; One producer's contribution; mirrors TRuntimeFilterBuildState.
  ;; Mode: 0 collecting      Keys holds every selected hash, repeats included,
  ;;                         until there are 2 * MaxKeys of them; one
  ;;                         compaction then tells mostly unique input from
  ;;                         duplicate-heavy input;
  ;;       1 deduplicating   Keys holds distinct hashes, at most MaxKeys;
  ;;       2 overflowing     more than MaxKeys distinct hashes wait for Bloom
  ;;                         storage;
  ;;       3 Bloom           Bloom holds every hash seen.
  ;; rf_build stops early when it needs storage it was not given and names
  ;; it in Want: 1 SortOrder, SortKeys and the set; 2 Bloom (BloomWords >= 0).
  ;; Keys always has room for the rows of the current batch.
  (type RuntimeFilterBuild <struct
    (MaxKeys i64)
    (Mode i64)
    (Want i64)
    (Keys <ptr u64>)
    (KeyCount i64)
    (SetCtrl <ptr u8>)
    (SetSlots <ptr u64>)
    (SetCapacity i64)
    (SortOrder <ptr u32>)
    (SortKeys <ptr u64>)
    (Bloom <ptr u64>)
    (BloomWords i64)>)

  ;; A finished contribution: exact keys, or a Bloom fragment.
  (type RuntimeFilterPartial <struct
    (Keys <ptr u64>)
    (KeyCount i64)
    (Bloom <ptr u64>)
    (BloomWords i64)>)

  ;; Storage and result of a merge; mirrors TRuntimeFilterPublishState.
  ;; Keys and SortKeys hold every partial key, SortOrder twice as many
  ;; entries plus 256. Table is zeroed and fits min(keys, MaxKeys) keys.
  ;; Bloom is zeroed, or with BloomSeeded already holds one fragment, which
  ;; makes the result a Bloom filter. Kind: 0 cannot answer, 1 exact, 2 Bloom.
  (type RuntimeFilterPublish <struct
    (MaxKeys i64)
    (Keys <ptr u64>)
    (SortOrder <ptr u32>)
    (SortKeys <ptr u64>)
    (Table <ptr u64>)
    (Bloom <ptr u64>)
    (BloomWords i64)
    (BloomSeeded i64)
    (Kind i64)
    (ExactKeys i64)
    (TableSize i64)
    (BloomSetBits i64)>)

  ;; The published filter. Words is the exact table or the Bloom words.
  (type RuntimeFilterData <struct
    (Kind i64)
    (Words <ptr u64>)
    (WordCount i64)>)

  ;; splitmix64 finalizer; one body serves u64 and <vec u64 4>.
  (fun rf_mix [T] ((var value T)) -> T (attrs inline)
    (block
      (= value (^ value (>> value (cast 30 u64))))
      (= value (* value (cast -4658895280553007687 u64)))
      (= value (^ value (>> value (cast 27 u64))))
      (= value (* value (cast -7723592293110705685 u64)))
      (return (^ value (>> value (cast 31 u64))))))

  (fun rf_selected ((var selection <ptr u8>) (var row i64)) -> bool (attrs inline)
    (block
      (if (== (cast selection i64) (: 0 i64))
        (block (return #t)))
      (return (!= (index selection row) (: 0 u8)))))

  ;; ---- Bloom ----
  ;; A hash selects one block and one bit in each of its eight words. Like
  ;; rf_mix, the three helpers below serve u64 and <vec u64 4>.

  ;; Offset of the block's first word.
  (fun rf_bloom_base [T] ((var block_mask u64) (var hash T)) -> T (attrs inline)
    (block
      (return (* (& (call rf_mix hash) block_mask) (cast 8 u64)))))

  ;; Byte i of the result picks the bit of word i.
  (fun rf_bloom_bits [T] ((var hash T)) -> T (attrs inline)
    (block
      (return (call rf_mix (^ hash (cast -7046029254386353131 u64))))))

  (fun rf_bloom_bit [T] ((var bits T) (var word i64)) -> T (attrs inline)
    (block
      (return (<< (cast 1 u64)
                (& (>> bits (cast (* word 8) u64)) (cast 63 u64))))))

  (fun rf_bloom_block_mask ((var word_count i64)) -> u64 (attrs inline)
    (block
      (return (cast (- (// word_count 8) 1) u64))))

  (fun rf_bloom_contains
       ((var words <ptr u64>) (var block_mask u64) (var hash u64)) -> bool
       (attrs inline)
    (block
      (var base = (cast (call rf_bloom_base block_mask hash) i64))
      (var bits = (call rf_bloom_bits hash))
      (var word = 0)
      (while (< word 8)
        (block
          (if (== (& (index words (+ base word)) (call rf_bloom_bit bits word))
                  (: 0 u64))
            (block (return #f)))
          (= word (+ word 1))))
      (return #t)))

  ;; Scalar OR: several hashes may set bits in one word, and every bit must
  ;; survive.
  (fun rf_bloom_add
       ((var words <ptr u64>) (var block_mask u64)
        (var hashes <ptr u64>) (var selection <ptr u8>) (var count i64))
    (block
      (var row = 0)
      (while (< row count)
        (block
          (if (call rf_selected selection row)
            (block
              (var hash = (index hashes row))
              (var base = (cast (call rf_bloom_base block_mask hash) i64))
              (var bits = (call rf_bloom_bits hash))
              (var word = 0)
              (while (< word 8)
                (block
                  (var slot = (+ base word))
                  (= words [slot]
                     (| (index words slot) (call rf_bloom_bit bits word)))
                  (= word (+ word 1))))))
          (= row (+ row 1))))))

  ;; Fragments of one filter use the same bit positions, so they merge by OR.
  (fun rf_bloom_or
       ((var target <ptr u64>) (var source <ptr u64>) (var count i64))
    (block
      (var i = 0)
      (while (< i count)
        (block
          (= target [i] (| (index target i) (index source i)))
          (= i (+ i 1))))))

  (fun rf_bloom_popcount ((var words <ptr u64>) (var count i64)) -> i64
    (block
      (var bits = 0)
      (var i = 0)
      (while (< i count)
        (block
          (= bits (+ bits (call builtin::ctpop (index words i))))
          (= i (+ i 1))))
      (return bits)))

  (fun rf_bloom_probe_scalar
       ((var words <ptr u64>) (var block_mask u64) (var hashes <ptr u64>)
        (var selection <ptr u8>) (var output <ptr u8>) (var count i64)) -> i64
    (block
      (var kept = 0)
      (var row = 0)
      (while (< row count)
        (block
          (var keep = (&& (call rf_selected selection row)
                          (call rf_bloom_contains words block_mask
                            (index hashes row))))
          (= output [row] (cast keep u8))
          (if keep
            (block (= kept (+ kept 1))))
          (= row (+ row 1))))
      (return kept)))

  ;; Four hashes per step over every row; the tail goes to the scalar probe.
  (fun rf_bloom_probe_dense
       ((var words <ptr u64>) (var block_mask u64) (var hashes <ptr u64>)
        (var output <ptr u8>) (var count i64)) -> i64
    (block
      (var vector_count = (& count (: -4 i64)))
      (var kept = 0)
      (var row = 0)
      (while (< row vector_count)
        (block
          (var hash = (vec (index hashes row)
                           (index hashes (+ row 1))
                           (index hashes (+ row 2))
                           (index hashes (+ row 3))))
          (var base = (call rf_bloom_base block_mask hash))
          (var bits = (call rf_bloom_bits hash))
          (var alive = (vec #t #t #t #t))
          (var word = 0)
          (while (&& (< word 8) (call builtin::any alive))
            (block
              (var found = (call builtin::gather words
                             (+ base (cast word u64))))
              (= alive (& alive (!= (& found (call rf_bloom_bit bits word))
                                    (cast 0 u64))))
              (= word (+ word 1))))
          (= output [row] (cast (index alive 0) u8))
          (= output [(+ row 1)] (cast (index alive 1) u8))
          (= output [(+ row 2)] (cast (index alive 2) u8))
          (= output [(+ row 3)] (cast (index alive 3) u8))
          (= kept (+ kept (call builtin::ctpop
                            (cast (call builtin::movemask alive) u64))))
          (= row (+ row 4))))
      (if (< row count)
        (block
          (var tail_hashes = (cast (+ (cast hashes i64) (* row 8)) <ptr u64>))
          (var tail_output = (cast (+ (cast output i64) row) <ptr u8>))
          (= kept (+ kept (call rf_bloom_probe_scalar words block_mask
                            tail_hashes (cast (: 0 i64) <ptr u8>) tail_output
                            (- count row))))))
      (return kept)))

  ;; Vector probe. A selection is first packed away, so that vector lanes
  ;; carry only rows that are still alive, and the answers are spread back
  ;; afterwards. selected_hashes and selected_output are scratch for count
  ;; entries each; they are not touched without a selection.
  (fun rf_bloom_probe_vector
       ((var words <ptr u64>) (var block_mask u64) (var hashes <ptr u64>)
        (var selection <ptr u8>) (var output <ptr u8>) (var count i64)
        (var selected_hashes <ptr u64>) (var selected_output <ptr u8>)) -> i64
    (block
      (if (== (cast selection i64) (: 0 i64))
        (block
          (return (call rf_bloom_probe_dense words block_mask hashes output
                    count))))
      (var selected = 0)
      (var row = 0)
      (while (< row count)
        (block
          (if (!= (index selection row) (: 0 u8))
            (block
              (= selected_hashes [selected] (index hashes row))
              (= selected (+ selected 1))))
          (= row (+ row 1))))
      (var kept = (call rf_bloom_probe_dense words block_mask selected_hashes
                    selected_output selected))
      (= selected 0)
      (= row 0)
      (while (< row count)
        (block
          (if (!= (index selection row) (: 0 u8))
            (block
              (= output [row] (index selected_output selected))
              (= selected (+ selected 1)))
            (block
              (= output [row] (: 0 u8))))
          (= row (+ row 1))))
      (return kept)))

  ;; ---- Exact set ----
  ;; Open addressing with linear probing over raw hashes. Zero marks an empty
  ;; slot, so hash 0 is stored as 1; mask is (capacity - 1).

  (fun rf_exact_stored ((var hash u64)) -> u64 (attrs inline)
    (block
      (if (== hash (: 0 u64))
        (block (return (: 1 u64))))
      (return hash)))

  ;; The slot holding hash, or the empty slot where it belongs.
  (fun rf_exact_slot
       ((var table <ptr u64>) (var mask u64) (var hash u64)) -> i64
       (attrs inline)
    (block
      (var stored = (call rf_exact_stored hash))
      (var slot = (cast (& hash mask) i64))
      (var found = (index table slot))
      (while (&& (!= found (: 0 u64)) (!= found stored))
        (block
          (= slot (cast (& (cast (+ slot 1) u64) mask) i64))
          (= found (index table slot))))
      (return slot)))

  (fun rf_exact_build
       ((var table <ptr u64>) (var mask u64)
        (var keys <ptr u64>) (var count i64))
    (block
      (var i = 0)
      (while (< i count)
        (block
          (var hash = (index keys i))
          (var slot = (call rf_exact_slot table mask hash))
          (= table [slot] (call rf_exact_stored hash))
          (= i (+ i 1))))))

  (fun rf_exact_probe
       ((var table <ptr u64>) (var mask u64) (var hashes <ptr u64>)
        (var selection <ptr u8>) (var output <ptr u8>) (var count i64)) -> i64
    (block
      (var kept = 0)
      (var row = 0)
      (while (< row count)
        (block
          (var keep = #f)
          (if (call rf_selected selection row)
            (block
              (var hash = (index hashes row))
              (= keep (== (index table (call rf_exact_slot table mask hash))
                          (call rf_exact_stored hash)))))
          (= output [row] (cast keep u8))
          (if keep
            (block (= kept (+ kept 1))))
          (= row (+ row 1))))
      (return kept)))

  ;; ---- Keys ----

  ;; Sorts keys and drops repeats in place; returns the distinct count.
  ;; Scratch: order and order_work hold count entries each, counts 256,
  ;; sorted count.
  (fun rf_keys_sort_unique
       ((var keys <ptr u64>) (var count i64)
        (var order <ptr u32>) (var order_work <ptr u32>)
        (var counts <ptr u32>) (var sorted <ptr u64>)) -> i64
    (block
      (var i = 0)
      (while (< i count)
        (block
          (= order [i] (cast i u32))
          (= i (+ i 1))))
      (call radix_sort_indices keys order order_work counts count (: 64 i64) #f)
      (= i 0)
      (while (< i count)
        (block
          (= sorted [i] (index keys (cast (index order i) i64)))
          (= i (+ i 1))))
      (var unique = 0)
      (= i 0)
      (while (< i count)
        (block
          (var key = (index sorted i))
          (if (|| (== unique 0) (!= key (index keys (- unique 1))))
            (block
              (= keys [unique] key)
              (= unique (+ unique 1))))
          (= i (+ i 1))))
      (return unique)))

  ;; ---- Deduplicating set ----
  ;; A swiss table of raw hashes: ctrl holds capacity control bytes (128 =
  ;; empty, otherwise the low seven bits of the mixed hash) and slots the
  ;; hashes. Capacity is a power of two and a multiple of the swiss group
  ;; width. The caller keeps the table at most half full, so every probe
  ;; sequence meets an empty slot.

  ;; #t when hash was absent and is now stored.
  (fun rf_set_insert
       ((var ctrl <ptr u8>) (var slots <ptr u64>) (var capacity i64)
        (var hash u64)) -> bool
    (block
      (var mixed = (call rf_mix hash))
      (var group_shift = (call swiss_group_shift))
      (var num_groups = (>> capacity group_shift))
      (var group_mask = (- num_groups (: 1 i64)))
      (var h2 = (& mixed (: 127 u64)))
      (var g = (& (cast (>> mixed (: 7 u64)) i64) group_mask))
      (var step = 0)
      (var probes = 0)
      (while (< probes num_groups)
        (block
          (var group = (call swiss_group ctrl g))
          (var m = (call swiss_match group h2))
          (while (!= m (: 0 u64))
            (block
              (var slot = (+ (<< g group_shift) (call swiss_lowest_index m)))
              (if (== (index slots slot) hash)
                (block (return #f)))
              (= m (call swiss_clear_first m))))
          (var empty = (call swiss_match_empty group))
          (if (!= empty (: 0 u64))
            (block
              (var slot = (+ (<< g group_shift)
                             (call swiss_lowest_index empty)))
              (= slots [slot] hash)
              (= ctrl [slot] (cast h2 u8))
              (return #t)))
          (= step (+ step 1))
          (= g (& (+ g step) group_mask))
          (= probes (+ probes 1))))
      (return #f)))

  ;; Resets the set to exactly the given distinct keys.
  (fun rf_set_fill
       ((var ctrl <ptr u8>) (var slots <ptr u64>) (var capacity i64)
        (var keys <ptr u64>) (var count i64))
    (block
      (var i = 0)
      (while (< i capacity)
        (block
          (= ctrl [i] (: 128 u8))
          (= i (+ i 1))))
      (= i 0)
      (while (< i count)
        (block
          (var inserted = (call rf_set_insert ctrl slots capacity
                            (index keys i)))
          (= i (+ i 1))))))

  ;; ---- Entry points ----

  ;; Adds the selected hashes to a producer's contribution. Returns the number
  ;; of rows consumed: all of them, unless state.Want asks for storage.
  (fun rf_build
       ((var state <ref RuntimeFilterBuild>) (var hashes <ptr u64>)
        (var selection <ptr u8>) (var count i64)) -> i64
    (block
      (var max_keys = (field state MaxKeys))
      (var keys = (field state Keys))
      (var row = 0)
      (field_assign state Want 0)
      (if (== (field state Mode) 0)
        (block
          (var limit = (* max_keys 2))
          (var used = (field state KeyCount))
          (while (&& (< row count) (< used limit))
            (block
              (if (call rf_selected selection row)
                (block
                  (= keys [used] (index hashes row))
                  (= used (+ used 1))))
              (= row (+ row 1))))
          (field_assign state KeyCount used)
          (if (< used limit)
            (block (return row)))
          (if (== (field state SetCapacity) 0)
            (block
              (field_assign state Want 1)
              (return row)))
          (var order = (field state SortOrder))
          (var order_work = (cast (+ (cast order i64) (* used 4)) <ptr u32>))
          (var counts = (cast (+ (cast order i64) (* used 8)) <ptr u32>))
          (= used (call rf_keys_sort_unique keys used order order_work counts
                    (field state SortKeys)))
          (field_assign state KeyCount used)
          (if (> used max_keys)
            (block
              (field_assign state Mode 2))
            (block
              (call rf_set_fill (field state SetCtrl) (field state SetSlots)
                (field state SetCapacity) keys used)
              (field_assign state Mode 1)))))
      (if (== (field state Mode) 1)
        (block
          (var ctrl = (field state SetCtrl))
          (var slots = (field state SetSlots))
          (var capacity = (field state SetCapacity))
          (var used = (field state KeyCount))
          (while (&& (< row count) (<= used max_keys))
            (block
              (if (call rf_selected selection row)
                (block
                  (var hash = (index hashes row))
                  (if (call rf_set_insert ctrl slots capacity hash)
                    (block
                      (= keys [used] hash)
                      (= used (+ used 1))))))
              (= row (+ row 1))))
          (field_assign state KeyCount used)
          (if (<= used max_keys)
            (block (return row)))
          (field_assign state Mode 2)))
      (var bloom = (field state Bloom))
      (var bloom_words = (field state BloomWords))
      (if (== (field state Mode) 2)
        (block
          (if (< bloom_words 0)
            (block
              (field_assign state Want 2)
              (return row)))
          (if (> bloom_words 0)
            (block
              (call rf_bloom_add bloom (call rf_bloom_block_mask bloom_words)
                keys (cast (: 0 i64) <ptr u8>) (field state KeyCount))))
          (field_assign state KeyCount 0)
          (field_assign state Mode 3)))
      ;; Bloom storage arrives between calls, so no row of this batch has
      ;; been consumed yet.
      (if (> bloom_words 0)
        (block
          (call rf_bloom_add bloom (call rf_bloom_block_mask bloom_words)
            hashes selection count)))
      (return count)))

  ;; Merges the contributions of every producer.
  (fun rf_publish
       ((var state <ref RuntimeFilterPublish>)
        (var partials <ptr RuntimeFilterPartial>) (var partial_count i64))
    (block
      (var bloom = (field state Bloom))
      (var bloom_words = (field state BloomWords))
      (var no_selection = (cast (: 0 i64) <ptr u8>))
      (field_assign state Kind 0)
      (if (!= (field state BloomSeeded) 0)
        (block
          (var block_mask = (call rf_bloom_block_mask bloom_words))
          (var i = 0)
          (while (< i partial_count)
            (block
              (var partial = (index partials i))
              (call rf_bloom_or bloom (field partial Bloom)
                (field partial BloomWords))
              (call rf_bloom_add bloom block_mask (field partial Keys)
                no_selection (field partial KeyCount))
              (= i (+ i 1))))
          (field_assign state Kind 2))
        (block
          (var keys = (field state Keys))
          (var total = 0)
          (var i = 0)
          (while (< i partial_count)
            (block
              (var partial = (index partials i))
              (var partial_keys = (field partial Keys))
              (var partial_key_count = (field partial KeyCount))
              (var k = 0)
              (while (< k partial_key_count)
                (block
                  (= keys [total] (index partial_keys k))
                  (= total (+ total 1))
                  (= k (+ k 1))))
              (= i (+ i 1))))
          ;; Sort once, after all producers finish, including duplicates
          ;; across lanes.
          (var order = (field state SortOrder))
          (var order_work = (cast (+ (cast order i64) (* total 4)) <ptr u32>))
          (var counts = (cast (+ (cast order i64) (* total 8)) <ptr u32>))
          (var distinct = (call rf_keys_sort_unique keys total order order_work
                            counts (field state SortKeys)))
          (if (<= distinct (field state MaxKeys))
            (block
              (var size = 8)
              (while (< size (* distinct 2))
                (block (= size (* size 2))))
              (call rf_exact_build (field state Table) (cast (- size 1) u64)
                keys distinct)
              (field_assign state TableSize size)
              (field_assign state ExactKeys distinct)
              (field_assign state Kind 1))
            (block
              (if (> bloom_words 0)
                (block
                  (call rf_bloom_add bloom
                    (call rf_bloom_block_mask bloom_words) keys no_selection
                    distinct)
                  (field_assign state Kind 2)))))))
      (if (== (field state Kind) 2)
        (block
          (field_assign state BloomSetBits
            (call rf_bloom_popcount bloom bloom_words))))))

  ;; Tests the selected hashes. A Bloom filter is probed four hashes at a
  ;; time when vectorized is nonzero; selected_hashes and selected_output
  ;; then are scratch for count entries each.
  (fun rf_probe
       ((var data <ref RuntimeFilterData>) (var hashes <ptr u64>)
        (var selection <ptr u8>) (var output <ptr u8>) (var count i64)
        (var vectorized i64)
        (var selected_hashes <ptr u64>) (var selected_output <ptr u8>)) -> i64
    (block
      (var words = (field data Words))
      (var word_count = (field data WordCount))
      (if (== (field data Kind) 1)
        (block
          (return (call rf_exact_probe words (cast (- word_count 1) u64)
                    hashes selection output count))))
      (var block_mask = (call rf_bloom_block_mask word_count))
      (if (!= vectorized 0)
        (block
          (return (call rf_bloom_probe_vector words block_mask hashes selection
                    output count selected_hashes selected_output))))
      (return (call rf_bloom_probe_scalar words block_mask hashes selection
                output count)))))
