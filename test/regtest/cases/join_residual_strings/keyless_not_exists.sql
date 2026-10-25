SELECT sa.k FROM sa WHERE NOT EXISTS (SELECT 1 FROM sb WHERE sb.s = sa.s || 'x');
