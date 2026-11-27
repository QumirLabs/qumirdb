SELECT sa.k FROM sa WHERE sa.s || 'x' IN (SELECT sb.s FROM sb);
