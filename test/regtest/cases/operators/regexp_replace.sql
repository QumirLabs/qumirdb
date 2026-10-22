SELECT REGEXP_REPLACE(url, '^https?://(?:www\.)?([^/]+)/.*$', '\1') AS domain
FROM t
WHERE REGEXP_REPLACE(url, '^https?://(?:www\.)?([^/]+)/.*$', '\1') <> 'blocked'
ORDER BY REGEXP_REPLACE(url, '^https?://(?:www\.)?([^/]+)/.*$', '\1');
