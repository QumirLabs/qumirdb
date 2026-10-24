(block
  ;; Backend selection is a constant emitted for the LLVM target machine:
  ;; 0 = portable SWAR, 1 = 8-byte NEON, 2 = 16-byte SSE2.
  ;; Every operation on one table uses this same geometry and mask format.
  (fun swiss_backend () -> i64
    (attrs inline)
    (block (return (call builtin::byte_match_backend))))

  (fun swiss_group_shift () -> i64
    (attrs inline)
    (block
      (if (== (call swiss_backend) 2) (block (return 4)))
      (return 3)))

  (fun swiss_group_width () -> i64
    (attrs inline)
    (block (return (<< (: 1 i64) (call swiss_group_shift)))))

  (fun swiss_group ((var ctrl <ptr u8>) (var group i64)) -> <ptr u8>
    (attrs inline)
    (block (return (cast (+ (cast ctrl i64)
      (<< group (call swiss_group_shift))) <ptr u8>))))

  ;; 8-byte masks carry a byte per slot (00/FF for NEON, 00/80 for SWAR).
  ;; SSE masks carry one bit per slot in the low sixteen bits.
  (fun swiss_match ((var ctrl <ptr u8>) (var h2 u64)) -> u64
    (attrs inline)
    (block
      (if (== (call swiss_backend) 2)
        (block (return (cast (call builtin::byte_match16 ctrl (cast h2 u8)) u64))))
      (if (== (call swiss_backend) 1)
        (block (return (call builtin::byte_match8 ctrl (cast h2 u8)))))
      (var words = (cast ctrl <ptr u64>))
      (var word = (index words 0))
      (var lsbs = (: 72340172838076673 u64))
      (var msbs = (<< lsbs (: 7 u64)))
      (var x = (^ word (* lsbs h2)))
      (return (& (- x lsbs) (^ msbs (& x msbs))))))

  ;; No tombstones: only empty bytes have their high bit set. Insertion and
  ;; rehash use a scalar AND on ARM/portable targets, without a SIMD transfer.
  (fun swiss_match_empty ((var ctrl <ptr u8>)) -> u64
    (attrs inline)
    (block
      (if (== (call swiss_backend) 2)
        (block (return (cast (call builtin::byte_match16 ctrl (: 128 u8)) u64))))
      (var words = (cast ctrl <ptr u64>))
      (var word = (index words 0))
      (var msbs = (<< (: 72340172838076673 u64) (: 7 u64)))
      (return (& word msbs))))

  (fun swiss_lowest_index ((var mask u64)) -> i64
    (attrs inline)
    (block
      (var index = (call builtin::cttz mask))
      (if (== (call swiss_backend) 2) (block (return index)))
      (return (>> index (: 3 i64)))))

  ;; Normalize FF bytes only when advancing past a rejected candidate. The
  ;; common successful first match avoids this AND entirely.
  (fun swiss_clear_first ((var mask u64)) -> u64
    (attrs inline)
    (block
      (if (== (call swiss_backend) 1)
        (block (= mask (& mask
          (<< (: 72340172838076673 u64) (: 7 u64))))))
      (return (& mask (- mask (: 1 u64)))))))
