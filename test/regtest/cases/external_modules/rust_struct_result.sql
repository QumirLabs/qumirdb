-- A Rust function returning a struct yields one column per field.
CREATE MODULE orbital LANGUAGE rust AS $$
#[repr(C)]
pub struct OrbitResult { pub root: f64, pub square: f64, pub positive: bool }
#[no_mangle]
pub extern "C" fn orbit_result(a: f64) -> OrbitResult {
  OrbitResult { root: a.sqrt(), square: a, positive: a > 0.0 }
}
$$;
CREATE FUNCTION orbit_result(a DOUBLE) RETURNS (DOUBLE, DOUBLE, BOOL)
SET MODULE TO orbital SET SYMBOL TO orbit_result;
SELECT orbit_result(a) FROM t;
