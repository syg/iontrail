function ParallelArray(buffer) {
  if (arguments.length === 0)
    buffer = %_SetNonBuiltinCallerInitObjectType([]);

  let buffer = %ToObject(buffer);
  // TODO: How do we check for Array-like?
  if (buffer.length >>> 0 !== buffer.length)
    %ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");

  // TODO: Private names.
  this.buffer = buffer;
}

%_MakeConstructible(ParallelArray);

function ParallelArrayMap(f) {
  let buffer = %_SetNonBuiltinCallerInitObjectType([]);
  buffer.length = 40;
  return %ParallelFillArray(buffer, function (buffer, id, n) {
    let slice = (buffer.length / n) | 0;
    buffer[slice * id] = id;
  });
}
