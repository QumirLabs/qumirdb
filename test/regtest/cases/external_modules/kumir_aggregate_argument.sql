-- The plan builder types the stddev_samp argument against the module catalog.
CREATE MODULE math LANGUAGE kumir AS $$
алг вещ kumir_root(вещ x)
нач
  знач := sqrt(x)
кон
$$;
SELECT stddev_samp(kumir_root(a)) AS d FROM t;
