SELECT sa.k, sb.k AS k2 FROM sa FULL JOIN sb ON sa.s || 'x' = sb.s;
