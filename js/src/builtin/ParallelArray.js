function ParallelArrayConstruct(buffer) {
  if (arguments.length === 0)
    buffer = %_SetNonBuiltinCallerInitObjectType([]);

  let buffer = %ToObject(buffer);
  // TODO: How do we check for Array-like?
  if (buffer.length >>> 0 !== buffer.length)
    %ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");

  // TODO: Private names.
  this.buffer = buffer;

  %_SetNonBuiltinCallerInitObjectType(this);
}

%_MakeConstructible(ParallelArray);

function ComputeTileBounds(len, id, n) {
  let slice = (len / n) | 0;
  let start = slice * id;
  let end = id === n - 1 ? len : slice * (id + 1);
  return [start, end];
}

function ParallelArrayMap(f) {
  function fill(buffer, id, n) {
    let [start, end] = ComputeTileBounds(buffer.length, id, n);
    buffer[0] = start;
    buffer[1] = end;
  }

  let buffer = %_SetNonBuiltinCallerInitObjectType([]);
  buffer.length = 40;

  if (!%ParallelFillArray(buffer, fill)) {
    let v = this.buffer;
    for (let i = 0; i < buffer.length; i++)
      buffer[i] = f(v[i]);
  }

  return new global.ParallelArray(buffer);
}
