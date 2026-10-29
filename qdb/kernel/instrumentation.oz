(block
  ;; Called only by opt-in task instrumentation, never from an operator's
  ;; ordinary row loop. Native and wasm use this same measurement program.
  (fun qdb_instrumentation_rows
       ((var batch <ref TRowSet>) (var widths <ptr i64>) (var output <ptr i64>))
    (block
      (var selection = (field batch Selection))
      (var columns = (field batch Columns))
      (var rows = 0)
      (var bytes = 0)
      (var i = 0)
      (while (< i (field batch RowCount))
        (block
          (var selected = #t)
          (if (!= (cast (field batch Selection) i64) 0)
            (block (= selected (!= (index selection i) 0))))
          (if selected
            (block
              (= rows (+ rows 1))
              (var c = 0)
              (while (< c (field batch ColumnCount))
                (block
                  (var col = (index columns c))
                  (var valid = #t)
                  (if (!= (cast (field col Mask) i64) 0)
                    (block (= valid (call bitoff (field col Mask) i
                      (cast (field col MaskBitOffset) i64)))))
                  (if valid
                    (block
                      (var width = (index widths c))
                      (if (< width 0)
                        (block
                          (if (== (field col OffsetWidth) 4)
                            (block
                              (var offsets = (cast (field col Offsets) <ptr i32>))
                              (= bytes (+ bytes (cast
                                (- (index offsets (+ i 1)) (index offsets i)) i64))))
                            (block
                              (var offsets = (field col Offsets))
                              (= bytes (+ bytes
                                (- (index offsets (+ i 1)) (index offsets i)))))))
                        (block (= bytes (+ bytes width))))))
                  (= c (+ c 1))))))
          (= i (+ i 1))))
      (= output [0] rows)
      (= output [1] bytes)))

  ;; Retained HashTable allocations at a batch boundary, including key arena,
  ;; join row-id buckets, and string reducer buffers. Temporary rehash copies,
  ;; allocator metadata, source rowsets and output buffers are not included.
  ;; output[3] is 1 for an aggregate table in the row layout: its arena
  ;; blocks and their list, its entries, which the capacity counts, and its
  ;; lookup scratch. Such a table also reports its RowRebuilds in output[4]
  ;; and its RowMovedFromSlots in output[5]; both are 0 otherwise.
  (fun qdb_instrumentation_hash
       ((var ht <ref HashTable>) (var join i64)
        (var string_buffers <ptr i64>) (var string_count i64)
        (var output <ptr i64>))
    (block
      (= output [3] 0)
      (= output [4] 0)
      (= output [5] 0)
      (var entries = (field (field ht RowTable) Capacity))
      (if (!= (cast (field (field ht RowTable) Entries) i64) 0)
        (block
          (= output [0] (field ht Size))
          (= output [1] entries)
          (= output [2] (+ (* entries 8)
            (* (field (field ht RowArena) BlockCount)
               (* (field (field ht RowArena) BlockRows) (field (field ht RowArena) RowBytes)))
            (* (field (field ht RowArena) BlockCapacity) 8)
            (* (field (field ht RowScratch) Capacity) (+ 48 (field ht KeySize)))))
          (= output [3] 1)
          (= output [4] (field ht RowRebuilds))
          (= output [5] (field ht RowMovedFromSlots))
          (return)))
      (var buffers = (field ht AggBuffers))
      (var blocks = (field ht OwnedBlocks))
      (var capacity = (field ht Capacity))
      (var size = (field ht Size))
      (var aggs = (field ht NumAggs))
      (var bytes = (+ (* capacity
        (+ (* (field ht KeySize) 2) 9 (* aggs 8))) (* aggs 8)
        (* (field ht OwnedBlockCapacity) 8)))
      (var i = 0)
      (while (< i (field ht OwnedBlockCount))
        (block
          (var header = (cast (index blocks i) <ptr i64>))
          (= bytes (+ bytes 24 (index header 1)))
          (= i (+ i 1))))
      (if (&& (!= join 0) (>= aggs 3))
        (block
          (var caps = (index buffers 1))
          (= i 0)
          (while (< i size)
            (block
              (= bytes (+ bytes (* (index caps i) 8)))
              (= i (+ i 1))))))
      ;; Reducer buffers are indexed by dense group: only the first Size
      ;; entries are initialized, a rehash copies no further.
      (= i 0)
      (while (< i string_count)
        (block
          (var metadata = (index buffers (index string_buffers i)))
          (var g = 0)
          (while (< g size)
            (block
              (= bytes (+ bytes (cast
                (>> (cast (index metadata g) u64) 32) i64)))
              (= g (+ g 1))))
          (= i (+ i 1))))
      (= output [0] size)
      (= output [1] capacity)
      (= output [2] bytes))))
