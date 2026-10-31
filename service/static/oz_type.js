const SCALAR_TYPES = new Set([
  'i8', 'i16', 'i32', 'i64',
  'u8', 'u16', 'u32', 'u64',
  'f32', 'f64', 'bool', 'string', 'char', 'void',
]);

function scalar(name) {
  if (!SCALAR_TYPES.has(name)) {
    throw new Error(`unknown oz scalar type: ${name}`);
  }
  return Object.freeze({ kind: 'scalar', name });
}

function named(name, args = []) {
  if (typeof name !== 'string' || !name.length) {
    throw new Error('oz named type needs a name');
  }
  if (!Array.isArray(args)) {
    throw new Error(`oz named type ${name} needs an argument array`);
  }
  return Object.freeze({ kind: 'named', name, args: Object.freeze([...args]) });
}

export const Type = Object.freeze({ scalar, named });

function printIdentifier(value) {
  return value.includes(' ') || value.includes('.') ? `|${value}|` : value;
}

function printGenericArg(arg) {
  if (typeof arg === 'number' && Number.isSafeInteger(arg)) {
    return String(arg);
  }
  if (arg && typeof arg === 'object') {
    return printType(arg);
  }
  throw new Error(`unsupported oz generic type argument: ${String(arg)}`);
}

export function printType(type) {
  if (!type || typeof type !== 'object') {
    throw new Error('oz type must be an object');
  }
  if (type.kind === 'scalar') {
    if (!SCALAR_TYPES.has(type.name)) {
      throw new Error(`unknown oz scalar type: ${type.name}`);
    }
    return type.name;
  }
  if (type.kind === 'named') {
    const args = type.args.length
      ? ` [${type.args.map(printGenericArg).join(' ')}]`
      : '';
    return `<named ${printIdentifier(type.name)}${args}>`;
  }
  throw new Error(`unsupported oz type kind: ${String(type.kind)}`);
}

function namedArgs(source, name) {
  const prefix = `<named ${name} [`;
  return source.startsWith(prefix) && source.endsWith(']>')
    ? source.slice(prefix.length, -2)
    : null;
}

export function prettifyType(type) {
  const source = String(type ?? '').trim();
  const nullable = namedArgs(source, 'Nullable');
  if (nullable !== null) {
    return `Nullable[${prettifyType(nullable)}]`;
  }
  const decimal = namedArgs(source, 'Decimal');
  if (decimal !== null) {
    return `Decimal[${decimal}]`;
  }
  return source;
}
