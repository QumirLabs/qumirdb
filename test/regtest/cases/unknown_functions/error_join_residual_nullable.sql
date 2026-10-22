SELECT sa.k FROM sa JOIN sb ON sa.k = sb.k AND nosuchfn(sa.k) = nosuchfn(sb.k);
