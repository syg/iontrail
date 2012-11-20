// FIXME: ICs must work in parallel.
// FIXME: Must be able to call native intrinsics in parallel with a JSContext,
//        or have native intrinsics provide both a sequential and a parallel
//        version.
// TODO: Multi-dimensional.
// TODO: Use let over var when Ion compiles let.
// TODO: Private names.
// XXX: Experiment with cloning the whole op.
// XXX: Hide buffer?

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
  function fill(result, id, n, f) {
    var [start, end] = ComputeTileBounds(result.length, id, n);
    for (var i = start; i < end; i++)
      result[i] = f(i);
  }

  if (length >>> 0 !== length)
    %ThrowError(JSMSG_BAD_ARRAY_LENGTH, "");

  var buffer = %ParallelBuildArray(length, fill, f);
  if (!buffer) {
    buffer = %_SetNonBuiltinCallerInitObjectType([]);
    buffer.length = length;
    fill(buffer, 0, 1, f);
  }

  var self = %_SetNonBuiltinCallerInitObjectType(this);
  self.buffer = buffer;
}

function ParallelArrayMap(f) {
  function fill(result, id, n, f, source) {
    var [start, end] = ComputeTileBounds(source.length, id, n);
    for (var i = start; i < end; i++)
      result[i] = f(source[i]);
  }

  var source = this.buffer;
  var length = source.length;

  var buffer = %ParallelBuildArray(length, fill, f, source);
  if (!buffer) {
    buffer = %_SetNonBuiltinCallerInitObjectType([]);
    fill(buffer, 0, 1, f, source);
  }

  return new global.ParallelArray(buffer);
}

// TODO: Reduce needs a new parallel intrinsic, maybe?
function ParallelArrayReduce(f) {
  var source = this.buffer;
  var length = source.length;

  if (length === 0)
    %ThrowError(JSMSG_PAR_ARRAY_REDUCE_EMPTY);

  // The accumulator: the objet petit a.
  //
  // "A VM's accumulator register is Objet petit a: the unattainable object
  // of desire that sets in motion the symbolic movement of interpretation."
  //     -- PLT Zizek
  var a = source[0];

  for (var i = 1; i < length; i++)
    a = f(a, source[i]);

  return a;
}

function ParallelArrayScan(f) {
  // TODO: Scan needs a new parallel intrinsic.
  function fill(result, f, source) {
    var a = source[0];

    for (var i = 1; i < source.length; i++) {
      a = f(a, source[i]);
      result[i] = a;
    }
  }

  var source = this.buffer;
  var length = source.length;

  if (length === 0)
    %ThrowError(JSMSG_PAR_ARRAY_REDUCE_EMPTY);

  var buffer = %_SetNonBuiltinCallerInitObjectType([]);
  fill(buffer, f, source);

  return new global.ParallelArray(buffer);
}

function ParallelArrayScatter(targets, zero, f, length) {
  // TODO: Parallelize. %ThrowError or any calling of intrinsics isn't safe.
  function fill(result, id, n, targets, zero, f, source) {
    var length = result.length;

    // Initialize a conflict array and initialize the result to the zero value.
    var conflict = [];
    var [start, end] = ComputeTileBounds(length, id, n);
    for (var i = start; i < end; i++) {
      result[i] = zero;
      conflict[i] = false;
    }

    var limit = length < targets.length ? length : targets.length;
    var [start, end] = ComputeTileBounds(limit, id, n);

    for (var i = start; i < end; i++) {
      var t = targets[i];

      if (t >>> 0 !== t)
        %ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, ".prototype.scatter");

      if (t >= length)
        %ThrowError(JSMSG_PAR_ARRAY_SCATTER_BOUNDS);

      if (conflict[t]) {
        if (!f)
          %ThrowError(JSMSG_PAR_ARRAY_SCATTER_CONFLICT);
        result[t] = f(source[i], result[t]);
      } else {
        result[t] = source[i];
        conflict[t] = true;
      }
    }
  }

  var source = this.buffer;

  if (targets.length >>> 0 !== targets.length)
    %ThrowError(JSMSG_BAD_ARRAY_LENGTH, "");
  if (length && length >>> 0 !== length)
    %ThrowError(JSMSG_BAD_ARRAY_LENGTH, "");

  var buffer = %_SetNonBuiltinCallerInitObjectType([]);
  buffer.length = length || source.length;
  fill(buffer, 0, 1, targets, zero, f, source);

  return new global.ParallelArray(buffer);
}

// TODO: Parallelize.
function ParallelArrayFilter(filters) {
  var source = this.buffer;
  var length = filters.length;

  if (length >>> 0 !== length)
    %ThrowError(JSMSG_BAD_ARRAY_LENGTH, "");

  var buffer = %_SetNonBuiltinCallerInitObjectType([]);

  for (var i = 0, pos = 0; i < length; i++) {
    if (filters[i])
      buffer[pos++] = source[i];
  }

  return new global.ParallelArray(buffer);
}

//
// Accessors and utilities.
//

function ParallelArrayGet(i) {
  return this.buffer[i];
}

function ParallelArrayLength() {
  return this.buffer.length;
}

function ParallelArrayToString() {
  return this.buffer.toString();
}
