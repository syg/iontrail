function ParallelArray(buffer) {
  if (arguments.length === 0)
    buffer = [];

  var buffer = %ToObject(buffer);
  // TODO: How do we check for Array-like?
  if (buffer.length >>> 0 !== buffer.length)
    %ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");

  // TODO: Private names.
  this.buffer = buffer;
}

%_MakeConstructible(ParallelArray);

function ParallelArrayMap(f) {
  var buffer = [];
  buffer.length = 40;
  return %ParallelFillArray(buffer, function (buffer, id, n) {
    var slice = (buffer.length / n) | 0;
    buffer[slice * id] = id;
  });
}
