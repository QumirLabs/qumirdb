SELECT ta.k FROM ta JOIN tb ON ta.k = tb.k AND nosuchfn(ta.k) = nosuchfn(tb.k);
