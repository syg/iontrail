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

function TruncateEnd(start, end) {
  var end1 = start + 3;
  if (end1 < end) return end1;
  return end;
}

function ComputeProducts(shape) {
  // Compute the partial products in reverse order.
  // e.g., if the shape is [A,B,C,D], then the
  // array |products| will be [1,D,CD,BCD].
  var product = 1;
  var products = [];
  var sdimensionality = shape.length;
  for (var i = sdimensionality - 1; i >= 0; i--) {
    products.push(product);
    product *= shape[i];
  }
  return products;
}

function ComputeIndices(shape, index1d) {
  // Given a shape and some index |index1d|, computes and returns an
  // array containing the N-dimensional index that maps to |index1d|.

  var products = ComputeProducts(shape);
  var l = shape.length;

  var result = [];
  for (var i = 0; i < l; i++) {
    // Obtain product of all higher dimensions.
    // So if i == 0 and shape is [A,B,C,D], yields BCD.
    var stride = products[l - i - 1];

    // Compute how many steps of width stride we could take.
    var index = (index1d / stride) | 0;
    result[i] = index;

    // Adjust remaining indices for smaller dimensions.
    index1d -= (index * stride);
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
  this.buffer = %_SetNonBuiltinCallerInitObjectType([]);
  this.offset = 0;
  this.shape = [0];
  this.get = ParallelArrayGet1;
}

function ParallelArrayConstruct1(buffer) {
  var buffer = %ToObject(buffer);
  var length = buffer.length >>> 0;
  if (length !== buffer.length)
    %ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");

  var buffer1 = %_SetNonBuiltinCallerInitObjectType([]);
  for (var i = 0; i < length; i++) {
    buffer1[i] = buffer[i];
  }

  this.buffer = buffer1;
  this.offset = 0;
  this.shape = [length];
  this.get = ParallelArrayGet1;
}

function ParallelArrayConstruct2(shape, f) {
  if (typeof shape === 'number') {
    return ParallelArrayBuild(this, [shape], f);
  } else {
    return ParallelArrayBuild(this, shape, f);
  }
}

function ParallelArrayConstruct3(shape, buffer, offset) {
  this.shape = shape;
  this.buffer = buffer;
  this.offset = offset;
  this.get = ParallelArrayGetN;

  if (shape.length == 1) {
    this.get = ParallelArrayGet1;
  } else if (shape.length == 2) {
    this.get = ParallelArrayGet2;
  } else if (shape.length == 3) {
    this.get = ParallelArrayGet3;
  }
}

function ParallelArrayBuild(self, shape, f) {
  self.shape = shape;
  self.offset = 0;

  if (shape.length === 1) {
    var length = shape[0];
    var buffer = %ParallelBuildArray(length, fill1, f);
    if (!buffer) {
      buffer = %_SetNonBuiltinCallerInitObjectType([]);
      buffer.length = length;
      fill1(buffer, 0, 1, false, f);
    }

    self.get = ParallelArrayGet1;
    self.buffer = buffer;
  } else if (shape.length === 2) {
    var length = shape[0] * shape[1];
    var buffer = %ParallelBuildArray(length, fill2, shape[1], f);
    if (!buffer) {
      buffer = %_SetNonBuiltinCallerInitObjectType([]);
      buffer.length = length;
      fill2(buffer, 0, 1, false, shape[1], f);
    }

    self.get = ParallelArrayGet2;
    self.buffer = buffer;
  } else if (shape.length == 3) {
    var length = shape[0] * shape[1] * shape[2];
    var buffer = %ParallelBuildArray(length, fill3, shape[1], shape[2], f);
    if (!buffer) {
      buffer = %_SetNonBuiltinCallerInitObjectType([]);
      buffer.length = length;
      fill3(buffer, 0, 1, false, shape[1], shape[2], f);
    }

    self.get = ParallelArrayGet3;
    self.buffer = buffer;
  } else {
    var length = 1;
    for (var i = 0; i < shape.length; i++) {
      length *= shape[i];
    }

    var buffer = %ParallelBuildArray(length, fillN, shape, f);
    if (!buffer) {
      buffer = %_SetNonBuiltinCallerInitObjectType([]);
      buffer.length = length;
      fillN(buffer, 0, 1, false, shape, f);
    }

    self.get = ParallelArrayGetN;
    self.buffer = buffer;
  }

  function fill1(result, id, n, warmup, f) {
    var [start, end] = ComputeTileBounds(result.length, id, n);
    if (warmup) { end = TruncateEnd(start, end); }
    for (var i = start; i < end; i++) {
      result[i] = f(i);
    }
  }

  function fill2(result, id, n, warmup, yw, f) {
    var [start, end] = ComputeTileBounds(result.length, id, n);
    if (warmup) { end = TruncateEnd(start, end); }
    var x = (start / yw) | 0;
    var y = start - x*yw;
    for (var i = start; i < end; i++) {
      result[i] = f(x, y);
      if (++y == yw) {
        y = 0;
        ++x;
      }
    }
  }

  function fill3(result, id, n, warmup, yw, zw, f) {
    var [start, end] = ComputeTileBounds(result.length, id, n);
    if (warmup) { end = TruncateEnd(start, end); }
    var x = (start / (yw*zw)) | 0;
    var r = start - x*yw*zw;
    var y = (r / zw) | 0;
    var z = r - y*zw;
    for (var i = start; i < end; i++) {
      result[i] = f(x, y, z);
      if (++z == zw) {
        z = 0;
        if (++y == yw) {
          y = 0;
          ++x;
        }
      }
    }
  }

  function fillN(result, id, n, warmup, shape, f) {
    // NB: In fact this will not currently be parallelized due to the
    // use of `f.apply()`.  But it's written as if it could be.  A guy
    // can dream, can't he?
    var [start, end] = ComputeTileBounds(result.length, id, n);
    if (warmup) { end = TruncateEnd(start, end); }
    var indices = ComputeIndices(shape, start);
    for (var i = start; i < end; i++) {
      result[i] = f.apply(null, indices);
      StepIndices(shape, indices);
    }
  }
}

function ParallelArrayMap(f) {
  function fill(result, id, n, warmup, self, f, length) {
    var [start, end] = ComputeTileBounds(length, id, n);
    if (warmup) { end = TruncateEnd(start, end); }
    for (var i = start; i < end; i++) {
      result[i] = f(self.get(i));
    }
  }

  var length = this.shape[0];
  var buffer = %ParallelBuildArray(length, fill, this, f, length);
  if (!buffer) {
    buffer = %_SetNonBuiltinCallerInitObjectType([]);
    fill(buffer, 0, 1, false, this, f, length);
  }
  return new global.ParallelArray([buffer.length], buffer, 0);
}

function ParallelArrayReduce(f) {
  var length = this.shape[0];

  if (length === 0)
    %ThrowError(JSMSG_PAR_ARRAY_REDUCE_EMPTY);

  ///////////////////////////////////////////////////////////////////////////
  // Parallel Version

  var slices = %ParallelSlices();
  if (length > slices) {
    // Attempt parallel reduction, but only if there is at least one
    // element per thread.  Otherwise the various slices having to
    // reduce empty spans of the source array.
    var subreductions = %ParallelBuildArray(slices, fill, this, f);
    if (subreductions) {
      // can't use reduce because subreductions is an array, not a
      // parallel array:
      var a = subreductions[0];
      for (var i = 1; i < subreductions.length; i++)
        a = f(a, subreductions[i]);
      return a;
    }
  }

  ///////////////////////////////////////////////////////////////////////////
  // Sequential Version

  return reduce(this, 0, length, f);

  ///////////////////////////////////////////////////////////////////////////
  // Helpers

  function reduce(self, start, end, f) {
    // The accumulator: the objet petit a.
    //
    // "A VM's accumulator register is Objet petit a: the unattainable object
    // of desire that sets in motion the symbolic movement of interpretation."
    //     -- PLT Zizek
    var a = self.get(start);
    for (var i = start+1; i < end; i++)
      a = f(a, self.get(i));
    return a;
  }

  function fill(result, id, n, warmup, self, f) {
    var [start, end] = ComputeTileBounds(self.length, id, n);
    if (warmup) { end = TruncateEnd(start, end); }
    result[id] = reduce(self, start, end, f);
  }
}

function ParallelArrayScan(f) {
  var length = this.shape[0];

  if (length === 0)
    %ThrowError(JSMSG_PAR_ARRAY_REDUCE_EMPTY);

  ///////////////////////////////////////////////////////////////////////////
  // Parallel version

  var slices = %ParallelSlices();
  if (length > slices) { // Each worker thread will have something to do.
    // compute scan of each slice: see comment on phase1() below
    var phase1buffer = %ParallelBuildArray(length, phase1, this, f);

    if (phase1buffer) {
      // build intermediate array: see comment on phase2() below
      var intermediates = [];
      var [start, end] = ComputeTileBounds(length, 0, slices);
      var acc = phase1buffer[end-1];
      intermediates[0] = acc;
      for (var i = 1; i < slices - 1; i++) {
        [start, end] = ComputeTileBounds(length, i, slices);
        acc = f(acc, phase1buffer[end-1]);
        intermediates[i] = acc;
      }

      // compute phase1 scan results with intermediates
      //
      // FIXME---if we had a %UnsafeWrite() primitive, we could reuse
      // the phase1buffer here and would not need to construct a new
      // buffer!
      var phase2buffer = %ParallelBuildArray(length, phase2,
                                             this, f, phase1buffer, intermediates);
      if (phase2buffer) {
        return new global.ParallelArray([length], phase2buffer, 0);
      }
    }
  }

  ///////////////////////////////////////////////////////////////////////////
  // Sequential version

  var buffer = %_SetNonBuiltinCallerInitObjectType([]);
  var acc = this.get(0);
  buffer[0] = acc;
  for (var i = 1; i < length; i++) {
    acc = f(acc, this.get(i));
    buffer[i] = acc;
  }
  return new global.ParallelArray([length], buffer, 0);

  ///////////////////////////////////////////////////////////////////////////
  // Helpers

  function phase1(result, id, n, warmup, self, f) {
    // In phase 1, we divide the source array into n even slices and
    // compute scan on each slice sequentially as it were the entire
    // array.  This function is responsible for computing one of those
    // slices.
    //
    // So, if we have an array [A,B,C,D,E,F,G,H,I], n == 3, and our function
    // |f| is sum, then would wind up computing a result array like:
    //
    //     [A, A+B, A+B+C, D, D+E, D+E+F, G, G+H, G+H+I]
    //      ^~~~~~~~~~~~^  ^~~~~~~~~~~~^  ^~~~~~~~~~~~~^
    //      Slice 0        Slice 1        Slice 2
    //
    // Read on in phase2 to see what we do next!
    var [start, end] = ComputeTileBounds(self.shape[0], id, n);
    if (warmup) { end = TruncateEnd(start, end); }
    var acc = self.get(start);
    result[start] = acc;
    for (var i = start + 1; i < end; i++) {
      var elem = self.get(i);
      acc = f(acc, elem);
      result[i] = acc;
    }
  }

  function phase2(result, id, n, warmup, self, f, phase1buffer, intermediates) {
    // After computing the phase1 results, we compute an
    // |intermediates| array.  |intermediates[i]| contains the result
    // of reducing the final value from each preceding slice j<i with
    // the final value of slice i.  So, to continue our previous
    // example, the intermediates array would contain:
    //
    //   [A+B+C, (A+B+C)+(D+E+F), ((A+B+C)+(D+E+F))+(G+H+I)]
    //
    // Here I have used parenthesization to make clear the order of
    // evaluation in each case.
    //
    //   An aside: currently the intermediates array is computed
    //   sequentially.  In principle, we could compute it in parallel,
    //   at the cost of doing duplicate work.  This did not seem
    //   particularly advantageous to me, particularly as the number
    //   of slices is typically quite small (one per core), so I opted
    //   to just compute it sequentially.
    //
    // Phase 2 contains the results of phase1 with the intermediates
    // array to produce the final scan results.  The idea is to
    // iterate over each element S[i] in the slice |id|, which
    // currently contains the result of reducing with S[0]...S[i]
    // (where S0 is the first thing in the slice), and combine that
    // with |intermediate[id-1]|, which represents the result of
    // reducing everything in the input array prior to the slice.
    //
    // To continue with our example, in phase 1 we computed slice 1 to
    // be [D, D+E, D+E+F].  We will combine those results with
    // |intermediates[1-1]|, which is |A+B+C|, so that the final
    // result is [(A+B+C)+D, (A+B+C)+(D+E), (A+B+C)+(D+E+F)].  Again I
    // am using parentheses to clarify how these results were reduced.
    //
    // These is one subtle point here!

    var [start, end] = ComputeTileBounds(self.shape[0], id, n);
    if (warmup) { end = TruncateEnd(start, end); }
    if (id == 0) {
      for (var i = start; i < end; i++) {
        result[i] = phase1buffer[i];
      }

      // NB: this setup is not great for warmups, since it means that
      // the code below is never executed during warmup mode.  Unfortunately,
      // it's not clear how best to fake the situation.
    } else {
      var intermediate_idx = id - 1;
      var intermediate = intermediates[intermediate_idx];
      for (var i = start; i < end; i++) {
        result[i] = f(intermediate, phase1buffer[i]);
      }
    }
  }
}

function ParallelArrayScatter(targets, zero, f, length) {
  // TODO: N-dimensional
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

function ParallelArrayFilter(filters) {
  var length = this.shape[0];

  if (filters.length >>> 0 !== length)
    %ThrowError(JSMSG_BAD_ARRAY_LENGTH, "");

  ///////////////////////////////////////////////////////////////////////////
  // Parallel version
  var slices = %ParallelSlices();
  if (length > slices) {
    var keepers = %ParallelBuildArray(slices, count_keepers, filters);
    if (keepers) {
      var total = 0;
      for (var i = 0; i < keepers.length; i++)
        total += keepers[i];
      var buffer = %ParallelBuildArray(total, copy_keepers, this, filters, keepers);
      if (buffer) {
        return new global.ParallelArray([total], buffer, 0);
      }
    }
  }

  ///////////////////////////////////////////////////////////////////////////
  // Sequential version
  var buffer = %_SetNonBuiltinCallerInitObjectType([]);
  for (var i = 0, pos = 0; i < length; i++) {
    if (filters[i])
      buffer[pos++] = this.get(i);
  }
  return new global.ParallelArray(buffer);

  function count_keepers(result, id, n, warmup, filters) {
    var [start, end] = ComputeTileBounds(filters.length, id, n);
    if (warmup) { end = TruncateEnd(start, end); }
    var count = 0;
    for (var i = start; i < end; i++) {
      if (filters[i])
        count++;
    }
    result[id] = count;
  }

  function copy_keepers(result, id, n, warmup, self, filters, keepers) {
    var [start, end] = ComputeTileBounds(filters.length, id, n);
    if (warmup) { end = TruncateEnd(start, end); }

    var pos = 0;
    for (var i = 0; i < id; i++)
      pos += keepers[i];

    for (var i = start; i < end; i++) {
      if (filters[i])
        result[pos++] = self.get(i);
    }
  }
}

function ParallelArrayPartition(amount) {
  if (amount >>> 0 !== amount)
    %ThrowError(JSMSG_BAD_ARRAY_LENGTH, ""); // XXX

  var length = this.shape[0];
  var partitions = (length / amount) | 0;

  if (partitions * amount !== length)
    %ThrowError(JSMSG_BAD_ARRAY_LENGTH, ""); // XXX

  var shape = [partitions, amount];
  for (var i = 1; i < this.shape.length; i++)
    shape.push(this.shape[i]);
  return new global.ParallelArray(shape, this.buffer, this.offset);
}

function ParallelArrayFlatten() {
  if (this.shape.length < 2)
    %ThrowError(JSMSG_BAD_ARRAY_LENGTH, ""); // XXX

  var shape = [this.shape[0] * this.shape[1]];
  for (var i = 2; i < this.shape.length; i++)
    shape.push(this.shape[i]);
  return new global.ParallelArray(shape, this.buffer, this.offset);
}

//
// Accessors and utilities.
//

function ParallelArrayGet1(i) {
  var udef; // For some reason `undefined` doesn't work
  if (i === udef) {
    return this;
  } else {
    return this.buffer[this.offset + i];
  }
}

function ParallelArrayGet2(x, y) {
  var udef; // For some reason `undefined` doesn't work
  var xw = this.shape[0];
  var yw = this.shape[1];
  if (x === udef) {
    return this;
  } else if (x >= xw) {
    return udef;
  } else if (y === udef) {
    return new global.ParallelArray([yw], this.buffer, this.offset + x*yw);
  } else if (y >= yw) {
    return udef;
  } else {
    var offset = y + x*yw;
    return this.buffer[this.offset + offset];
  }
}

function ParallelArrayGet3(x, y, z) {
  var udef; // For some reason `undefined` doesn't work
  var xw = this.shape[0];
  var yw = this.shape[1];
  var zw = this.shape[2];
  if (x === udef) {
    return this;
  } else if (x >= xw) {
    return udef;
  } else if (y === udef) {
    return new global.ParallelArray([yw, zw], this.buffer, this.offset + x*yw*zw);
  } else if (y >= yw) {
    return udef;
  } else if (z === udef) {
    return new global.ParallelArray([zw], this.buffer, this.offset + y*zw + x*yw*zw);
  } else if (z >= zw) {
    return udef;
  } else {
    var offset = z + y*zw + x*yw*zw;
    return this.buffer[this.offset + offset];
  }
}

function ParallelArrayGetN(...coords) {
  var udef; // For some reason `undefined` doesn't work
  if (coords.length == 0)
    return this;

  var products = ComputeProducts(this.shape);

  // Compute the offset of the given coordinates.  Each index is
  // multipled by its corresponding entry in the |products|
  // array, counting in reverse.  So if |coords| is [a,b,c,d],
  // then you get |a*BCD + b*CD + c*D + d|.
  var offset = this.offset;
  var sdimensionality = this.shape.length;
  var cdimensionality = coords.length;
  for (var i = 0; i < cdimensionality; i++) {
    if (coords[i] >= this.shape[i])
      return udef;
    offset += coords[i] * products[sdimensionality - i - 1];
  }

  if (cdimensionality < sdimensionality) {
    var shape = this.shape.slice(cdimensionality);
    return new global.ParallelArray(shape, this.buffer, offset);
  } else {
    return this.buffer[offset];
  }
}

function ParallelArrayLength() {
  return this.shape[0];
}

function ParallelArrayToString() {
  var l = this.shape[0];
  if (l == 0)
    return "<>";

  var result = "<";
  for (var i = 0; i < l - 1; i++) {
    result += this.get(i).toString();
    result += ",";
  }
  result += this.get(l-1).toString();
  result += ">";
  return result;
}

// Unit Test Functions
//
// function CheckIndices(shape, index1d) {
//   let idx = ComputeIndices(shape, index1d);
//
//   let c = 0;
//   for (var i = 0; i < shape.length; i++) {
//     var stride = 1;
//     for (var j = i + 1; j < shape.length; j++) {
//       stride *= shape[j];
//     }
//     c += idx[i] * stride;
//   }
//   
//   assertEq(index1d, c);
// }
// 
// for (var q = 0; q < 2*4*6*8; q++) {
//   CheckIndices([2,4,6,8], q);
// }
