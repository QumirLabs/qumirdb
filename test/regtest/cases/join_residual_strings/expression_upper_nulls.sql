SELECT sa.k, sb.k AS k2 FROM sa JOIN sb ON upper(sa.s) = upper(substr(sb.s, 1, 1));
