SELECT id, body FROM texts
WHERE flag = 1 AND body LIKE '%needle%'
ORDER BY id;
