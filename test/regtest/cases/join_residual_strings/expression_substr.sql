SELECT sa.k, sb.k AS k2 FROM sa JOIN sb ON substr(sa.s, 1, 1) = substr(sb.s, 1, 1);
