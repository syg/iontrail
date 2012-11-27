// FIXME: ICs must work in parallel.
// FIXME: Must be able to call native intrinsics in parallel with a JSContext,
//        or have native intrinsics provide both a sequential and a parallel
//        version.
// TODO: Multi-dimensional.
// TODO: Use let over var when Ion compiles let.
// TODO: Private names.
// XXX: Experiment with cloning the whole op.
// XXX: Hide buffer and other fields?

function ComputeTileBounds(len, id, n) {
  var slice = (len / n) | 0;
  var start = slice * id;
  var end = id === n - 1 ? len : slice * (id + 1);
  return [start, end];
}

function ComputeProducts(shape) {
  var l = shape.length;
  var products = [];
  var product = 1;
  for (var i = 0; i < l; i++) {
    products[i] = product;
    product = product * shape[i];
  }
  return products;
}

function ComputeIndices(shape, index1d) {
  var products = ComputeProducts(shape);
  var l = shape.length;

  var result = [];
  for (var i = 0; i < l; i++) {
    var stride = products[l - i - 1];
    var index = (index1d / stride) | 0;
    index1d -= (index * stride);
    result[i] = index;
  }
  return result;
}

function StepIndices(shape, indices) {
  var i = shape.length - 1;
  while (i >= 0) {
    var indexi = indices[i] + 1;
    if (indexi < shape[i]) {
      indices[i] = indexi;
      return;
    }
    indices[i] = 0;
    i--;
  }
}

// Constructor
//
// We split the 3 construction cases so that we don't case on arguments, which
// deoptimizes.

function ParallelArrayConstruct0() {
  var self = %_SetNonBuiltinCallerInitObjectType(this);
  self.buffer = %_SetNonBuiltinCallerInitObjectType([]);
  self.bufferOffset = 0;
  self.shape = [0];
  self.get = ParallelArrayGet1;
}

function ParallelArrayConstruct1(buffer) {
  var buffer = %ToObject(buffer);
  // TODO: How do we check for Array-like?
  if (buffer.length >>> 0 !== buffer.length)
    %ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");

  var self = %_SetNonBuiltinCallerInitObjectType(this);
  self.buffer = buffer;
  self.bufferOffset = 0;
  self.shape = [buffer.length];
  self.get = ParallelArrayGet1;
}

function ParallelArrayConstruct2(shape, f) {
  return ParallelArrayBuild(this, shape, f);
}

function ParallelArrayBuild(self0, shape, f) {
  function fill(result, id, n, shape, f) {
    var [start, end] = ComputeTileBounds(result.length, id, n);
    var indices = ComputeIndices(shape, start);
    for (var i = start; i < end; i++) {
      result[i] = f.apply(null, indices);
      StepIndices(shape, indices);
    }
  }

  var length = 1;
  for (var i = 0; i < shape.length; i++) {
    length *= shape[i];
  }

  var buffer = %ParallelBuildArray(length, fill, shape, f);
  if (!buffer) {
    buffer = %_SetNonBuiltinCallerInitObjectType([]);
    buffer.length = length;
    fill(buffer, 0, 1, shape, f);
  }

  var self = %_SetNonBuiltinCallerInitObjectType(self0);
  self.shape = shape;
  self.bufferOffset = 0;
  self.get = ParallelArrayGetN;
  self.buffer = buffer;

  if (self.shape.length == 1) {
    self.get = ParallelArrayGet1;
  } else if (self.shape.length == 2) {
    self.get = ParallelArrayGet2;
  } else if (self.shape.length == 3) {
    self.get = ParallelArrayGet3;
  }
}

function ParallelArrayMap(f) {
  function fill(result, id, n, f, shape, source) {
    var [start, end] = ComputeTileBounds(source.length, id, n);

    var stride = 1;
    for (var i = 0; i < shape.length - 1; i++)
      stride *= shape[i];

    for (var i = start; i < end; i++) {
      result[i] = f(source[i]);
    }
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

function ParallelArrayReduce(f) {
  var source = this.buffer;
  var length = source.length;

  if (length === 0)
    %ThrowError(JSMSG_PAR_ARRAY_REDUCE_EMPTY);

  function reduce(source, start, end, f) {
    // The accumulator: the objet petit a.
    //
    // "A VM's accumulator register is Objet petit a: the unattainable object
    // of desire that sets in motion the symbolic movement of interpretation."
    //     -- PLT Zizek
    var a = source[start];
    for (var i = start+1; i < end; i++)
      a = f(a, source[i]);
    return a;
  }

  function fill(result, id, n, source, f) {
    // Mildly awkward: in the real parallel phase, there will be
    // precisely one entry in result per worker.  But in the warmup
    // phase, that is not so!  Therefore, we store the reduced version
    // into |id % result.length| so as to ensure that in the warmup
    // phase |id| never exceeds the length of result.
    var [start, end] = ComputeTileBounds(source.length, id, n);
    //result[id % result.length] = reduce(source, start, end, f);

    var a = source[start];
    for (var i = start+1; i < end; i++)
      a = f(a, source[i]);
    result[id % result.length] = a;
  }

  var threads = %_GetThreadPoolInfo().numThreads;
  var subreductions = %ParallelBuildArray(threads, fill, source, f);
  if (subreductions) {
    return reduce(subreductions, 0, subreductions.length, f);
  } else {
    return reduce(source, 0, length, f);
  }
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

function ParallelArrayGet1(i) {
  return this.buffer[this.bufferOffset + i];
}

function ParallelArrayGet2(x, y) {
  var yw = this.shape[1];
  var offset = y + yw * x;
  return this.buffer[this.bufferOffset + offset];
}

function ParallelArrayGet3(x, y, z) {
  var yw = this.shape[1];
  var zw = this.shape[2];
  var offset = z + zw * y + zw * yw * x;
  return this.buffer[this.bufferOffset + offset];
}

function ParallelArrayGetN() {
  var products = ComputeProducts(self.shape);
  var offset = 0;
  var dimensionality = self.shape.length;
  for (var i = 0; i < dimensionality; i++) {
    offset += arguments[i] * products[dimensionality - i - 1];
  }
  return this.buffer[this.bufferOffset + offset];
}

function ParallelArrayLength() {
  return this.buffer.length;
}

function ParallelArrayToString() {
  return this.buffer.toString();
}
