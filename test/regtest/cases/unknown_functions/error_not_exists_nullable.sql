SELECT sa.k FROM sa WHERE NOT EXISTS
    (SELECT 1 FROM sb WHERE sb.k = sa.k AND nosuchfn(sb.k) = sa.k);
