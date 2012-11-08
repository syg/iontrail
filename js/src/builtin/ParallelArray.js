// TODO: Use let over var when Ion compiles let.
// TODO: Private names.

function ComputeTileBounds(len, id, n) {
  var slice = (len / n) | 0;
  var start = slice * id;
  var end = id === n - 1 ? len : slice * (id + 1);
  return [start, end];
}

// Constructor
//
// We split the 3 construction cases so that we don't case on arguments, which
// deoptimizes.

function ParallelArrayConstruct0() {
  var self = %_SetNonBuiltinCallerInitObjectType(this);
  self.buffer = %_SetNonBuiltinCallerInitObjectType([]);
}

function ParallelArrayConstruct1(buffer) {
  var buffer = %ToObject(buffer);
  // TODO: How do we check for Array-like?
  if (buffer.length >>> 0 !== buffer.length)
    %ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");

  var self = %_SetNonBuiltinCallerInitObjectType(this);
  self.buffer = buffer;
}

function ParallelArrayConstruct2(length, f) {
  // Per-thread worker.
  function fill(result, id, n) {
    var [start, end] = ComputeTileBounds(result.length, id, n);
    for (var i = start; i < end; i++)
      result[i] = f(i);
  }

  if (length >>> 0 !== length)
    %ThrowError(JSMSG_BAD_ARRAY_LENGTH, "");

  var buffer = %_SetNonBuiltinCallerInitObjectType([]);
  // FIXME: Hack to un-specialize the set since it's full of holes?
  buffer[0] = undefined;
  buffer.length = length;

  if (!%ParallelFillArray(buffer, fill)) {
    for (var i = 0; i < length; i++)
      buffer[i] = f(i);
  }

  var self = %_SetNonBuiltinCallerInitObjectType(this);
  self.buffer = buffer;
}

function ParallelArrayMap(f) {
  var source = this.buffer;
  return new global.ParallelArray(source.length, function (i) {
    return f(source[i]);
  });
}
