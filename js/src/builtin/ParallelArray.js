// TODO: Use let over var when Ion compiles let.

function ParallelArrayConstruct(buffer) {
  if (arguments.length === 0)
    buffer = %_SetNonBuiltinCallerInitObjectType([]);

  var buffer = %ToObject(buffer);
  // TODO: How do we check for Array-like?
  if (buffer.length >>> 0 !== buffer.length)
    %ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");

  // TODO: Private names.
  this.buffer = buffer;

  %_SetNonBuiltinCallerInitObjectType(this);
}

function ComputeTileBounds(len, id, n) {
  var slice = (len / n) | 0;
  var start = slice * id;
  var end = id === n - 1 ? len : slice * (id + 1);
  return [start, end];
}

function ParallelArrayMap(f) {
  // Allocate a new buffer and set it the same length as the source.
  var source = this.buffer;
  var result = %_SetNonBuiltinCallerInitObjectType([]);
  result.length = source.length;

  // Per-thread worker.
  function fill(result, id, n) {
    var [start, end] = ComputeTileBounds(result.length, id, n);
    for (var i = start; i < end; i++)
      result[i] = f(source[i]);
  }

  if (!%ParallelFillArray(result, fill)) {
    for (var i = 0; i < source.length; i++)
      result[i] = f(source[i]);
    }

  return new global.ParallelArray(result);
}
