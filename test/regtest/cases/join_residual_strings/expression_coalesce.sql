SELECT sa.k, sb.k AS k2 FROM sa JOIN sb ON coalesce(sa.s, 'x') = sb.s;
