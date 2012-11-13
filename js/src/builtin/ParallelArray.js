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

function Construct2Fill(result, id, n, f) {
  var [start, end] = ComputeTileBounds(result.length, id, n);
  for (var i = start; i < end; i++)
    result[i] = f(i);
}

function ParallelArrayConstruct2(length, f) {
  if (length >>> 0 !== length)
    %ThrowError(JSMSG_BAD_ARRAY_LENGTH, "");

  var fill = %KeyedCloneFunction(f, Construct2Fill);
  var buffer = %ParallelBuildArray(length, fill, f);
  if (!buffer) {
    buffer = %_SetNonBuiltinCallerInitObjectType([]);
    buffer.length = length;
    fill(buffer, 0, 1, f);
  }

  var self = %_SetNonBuiltinCallerInitObjectType(this);
  self.buffer = buffer;
}

function MapFill(result, id, n, f, source) {
  var [start, end] = ComputeTileBounds(result.length, id, n);
  for (var i = start; i < end; i++)
    result[i] = f(source[i]);
}

function ParallelArrayMap(f) {
  var source = this.buffer;
  var length = source.length;

  var fill = %KeyedCloneFunction(f, MapFill);
  var buffer = %ParallelBuildArray(length, fill, f, source);
  if (!buffer) {
    buffer = %_SetNonBuiltinCallerInitObjectType([]);
    buffer.length = length;
    fill(buffer, 0, 1, f, source);
  }

  return new global.ParallelArray(buffer);
}
