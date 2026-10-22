SELECT ta.k FROM ta WHERE NOT EXISTS
    (SELECT 1 FROM tb WHERE tb.k = ta.k AND nosuchfn(tb.k) = ta.k);
