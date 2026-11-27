SELECT sa.k FROM sa WHERE EXISTS (SELECT 1 FROM sb WHERE substr(sb.s, 1, 1) = sa.s);
