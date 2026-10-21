SELECT z, w
FROM u
WHERE w > (SELECT min(w) FROM u v WHERE z = u.z);
