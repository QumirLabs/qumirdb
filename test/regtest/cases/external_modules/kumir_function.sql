CREATE MODULE math LANGUAGE kumir AS $$
алг вещ kumir_root(вещ x)
нач
  знач := sqrt(x)
кон
$$;
SELECT kumir_root(a) FROM t;
