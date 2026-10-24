SELECT sa.k, sb.k AS k2 FROM sa LEFT JOIN sb ON sa.s || 'x' = sb.s;
