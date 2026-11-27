SELECT sa.k, sb.s FROM sa JOIN sb ON substr(sa.s, 1, 1) = substr(sb.s, 1, 1) AND sa.k = sb.k AND sa.k * 2 = sb.k + sb.k;
