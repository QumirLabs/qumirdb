SELECT sa.k, sb.s FROM sa JOIN sb ON sa.k = sb.k AND sa.s || 'x' = sb.s;
