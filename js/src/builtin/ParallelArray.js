// FIXME: ICs must work in parallel.
// FIXME: Must be able to call native intrinsics in parallel with a JSContext,
//        or have native intrinsics provide both a sequential and a parallel
//        version.
// TODO: Use let over var when Ion compiles let.
// TODO: Private names.
// XXX: Hide buffer and other fields?

function ComputeNumChunks(length) {
  // Determine the number of chunks of size CHUNK_SIZE;
  // note that the final chunk may be smaller than CHUNK_SIZE.
  var chunks = length >>> CHUNK_SHIFT;
  if (chunks << CHUNK_SHIFT === length)
    return chunks;
  return chunks + 1;
}

function ComputeSliceBounds(len, id, n) {
  // Computes the bounds for slice |id| of |len| items, assuming |n|
  // total slices.  If len is not evenly divisible by n, then the
  // final thread may have a bit of extra work.  It might be better to
  // do the division more equitably.
  var slice = (len / n) | 0;
  var start = slice * id;
  var end = id === n - 1 ? len : slice * (id + 1);
  return [start, end];
}

function ComputeAllSliceBounds(length, numSlices) {
  // Computes the bounds for all slices of |length| items, assuming
  // that there are |slices| items.  The result is an array containing
  // multiple values per slice: the start index, end index, current
  // position, and some padding.  The current position is initally the
  // same as the start index.  To access the values for a particular
  // slice, use the macros SLICE_START() and so forth.

  var info = [];
  for (var i = 0; i < numSlices; i++) {
    var [start, end] = ComputeSliceBounds(length, i, numSlices);
    info.push(SLICE_INFO(start, end));
  }
  return info;
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

function IsInteger(v) {
  return (v | 0) === v;
}

// I'd prefer to use Math.min, but we have no way to access a pristine
// copy.  global.Math.min might be modified by the user.
function IntMin(a, b) {
  var a1 = a | 0;
  var b1 = b | 0;
  return (a1 < b1 ? a1 : b1);
}

// Constructor
//
// We split the 3 construction cases so that we don't case on arguments.

function ParallelArrayConstruct0() {
  this.buffer = [];
  this.offset = 0;
  this.shape = [0];
  this.get = ParallelArrayGet1;
}

function ParallelArrayConstruct1(buffer) {
  var buffer = ToObject(buffer);
  var length = buffer.length >>> 0;
  if (length !== buffer.length)
    ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");

  var buffer1 = [];
  for (var i = 0; i < length; i++)
    buffer1[i] = buffer[i];

  this.buffer = buffer1;
  this.offset = 0;
  this.shape = [length];
  this.get = ParallelArrayGet1;
}

function ParallelArrayConstruct2(shape, f) {
  if (typeof shape === "number") {
    var length = shape >>> 0;
    if (length !== shape)
      ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");
    ParallelArrayBuild(this, [length], f);
  } else {
    var shape1 = [];
    for (var i = 0, l = shape.length; i < l; i++) {
      var s0 = shape[i];
      var s1 = s0 >>> 0;
      if (s1 !== s0)
        ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");
      shape1[i] = s1;
    }
    ParallelArrayBuild(this, shape1, f);
  }
}

// We duplicate code here to avoid extra cloning.
function ParallelArrayConstruct3(shape, f, m) {
  if (typeof shape === "number") {
    var length = shape >>> 0;
    if (length !== shape)
      ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");
    ParallelArrayBuild(this, [length], f, m);
  } else {
    var shape1 = [];
    for (var i = 0, l = shape.length; i < l; i++) {
      var s0 = shape[i];
      var s1 = s0 >>> 0;
      if (s1 !== s0)
        ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");
      shape1[i] = s1;
    }
    ParallelArrayBuild(this, shape1, f, m);
  }
}

function ParallelArrayView(shape, buffer, offset) {
  this.shape = shape;
  this.buffer = buffer;
  this.offset = offset;

  switch (shape.length) {
    case 1: this.get = ParallelArrayGet1; break;
    case 2: this.get = ParallelArrayGet2; break;
    case 3: this.get = ParallelArrayGet3; break;
    default: this.get = ParallelArrayGetN; break;
  }
}

function ParallelArrayBuild(self, shape, f, m) {
  self.offset = 0;
  self.shape = shape;

  var length;
  var xw, yw, zw;
  var computefunc;

  switch (shape.length) {
  case 1:
    length = shape[0];
    self.get = ParallelArrayGet1;
    computefunc = fill1;
    break;
  case 2:
    xw = shape[0];
    yw = shape[1];
    length = xw * yw;
    self.get = ParallelArrayGet2;
    computefunc = fill2;
    break;
  case 3:
    xw = shape[0];
    yw = shape[1];
    zw = shape[2];
    length = xw * yw * zw;
    self.get = ParallelArrayGet3;
    computefunc = fill3;
    break;
  default:
    length = 1;
    for (var i = 0; i < shape.length; i++)
      length *= shape[i];
    self.get = ParallelArrayGetN;
    computefunc = fillN;
    break;
  }

  var buffer = self.buffer = DenseArray(length);

  parallel: for (;;) { // see ParallelArrayMap() to explain why for(;;) etc
    if (ForceSequential())
      break parallel;
    if (!TRY_PARALLEL(m))
      break parallel;
    if (computefunc === fillN)
      break parallel;

    var chunks = ComputeNumChunks(length);
    var numSlices = ParallelSlices();
    if (chunks < numSlices)
      break parallel;
    var info = ComputeAllSliceBounds(chunks, numSlices);
    ParallelDo(constructSlice, CheckParallel(m));
    return;
  }

  computefunc(0, length);
  return;

  function constructSlice(id, n, warmup) {
    var chunkPos = info[SLICE_POS(id)];
    var chunkEnd = info[SLICE_END(id)];

    if (warmup && chunkEnd > chunkPos)
      chunkEnd = chunkPos + 1;

    while (chunkPos < chunkEnd) {
      var indexStart = chunkPos << CHUNK_SHIFT;
      var indexEnd = IntMin(indexStart + CHUNK_SIZE, length);
      computefunc(indexStart, indexEnd);
      UnsafeSetElement(info, SLICE_POS(id), ++chunkPos);
    }
  }

  function fill1(indexStart, indexEnd) {
    for (var i = indexStart; i < indexEnd; i++)
      UnsafeSetElement(buffer, i, f(i));
  }

  function fill2(indexStart, indexEnd) {
    var x = (indexStart / yw) | 0;
    var y = indexStart - x*yw;
    for (var i = indexStart; i < indexEnd; i++) {
      UnsafeSetElement(buffer, i, f(x, y));
      if (++y == yw) {
        y = 0;
        ++x;
      }
    }
  }

  function fill3(indexStart, indexEnd) {
    var x = (indexStart / (yw*zw)) | 0;
    var r = indexStart - x*yw*zw;
    var y = (r / zw) | 0;
    var z = r - y*zw;
    for (var i = indexStart; i < indexEnd; i++) {
      UnsafeSetElement(buffer, i, f(x, y, z));
      if (++z == zw) {
        z = 0;
        if (++y == yw) {
          y = 0;
          ++x;
        }
      }
    }
  }

  function fillN(indexStart, indexEnd) {
    var indices = ComputeIndices(shape, indexStart);
    for (var i = indexStart; i < indexEnd; i++) {
      UnsafeSetElement(buffer, i, f.apply(null, indices));
      StepIndices(shape, indices);
    }
  }
}

function ParallelArrayMap(f, m) {
  var self = this;
  var length = self.shape[0];
  var buffer = DenseArray(length);

  parallel: for (;;) {

    // Avoid parallel compilation if we are already nested in another
    // parallel section or the user told us not to.  The somewhat
    // artificial style of this code is working around some ion
    // limitations:
    //
    // - Breaking out of named blocks does not currently work;
    // - Unreachable Code Elim. can't properly handle if (a && b)
    if (ForceSequential())
      break parallel;
    if (!TRY_PARALLEL(m))
      break parallel;

    var chunks = ComputeNumChunks(length);
    var numSlices = ParallelSlices();

    // At the moment, there must be at least one chunk per slice or
    // warmup sometimes fails, leading to the fill fn to be
    // permanently excluded from parallel compilation. This is really
    // a bug in our handling of failed compilation though.
    if (chunks < numSlices)
      break parallel;

    var info = ComputeAllSliceBounds(chunks, numSlices);
    ParallelDo(mapSlice, CheckParallel(m));
    return NewParallelArray(ParallelArrayView, [length], buffer, 0);
  }

  // Sequential fallback:
  for (var i = 0; i < length; i++)
    buffer[i] = f(self.get(i), i, self);
  return NewParallelArray(ParallelArrayView, [length], buffer, 0);

  function mapSlice(id, n, warmup) {
    var chunkPos = info[SLICE_POS(id)];
    var chunkEnd = info[SLICE_END(id)];

    if (warmup && chunkEnd > chunkPos)
      chunkEnd = chunkPos + 1;

    while (chunkPos < chunkEnd) {
      var indexStart = chunkPos << CHUNK_SHIFT;
      var indexEnd = IntMin(indexStart + CHUNK_SIZE, length);

      for (var i = indexStart; i < indexEnd; i++)
        UnsafeSetElement(buffer, i, f(self.get(i), i, self));

      UnsafeSetElement(info, SLICE_POS(id), ++chunkPos);
    }
  }
}

function ParallelArrayReduce(f, m) {
  var self = this;
  var length = self.shape[0];

  if (length === 0)
    ThrowError(JSMSG_PAR_ARRAY_REDUCE_EMPTY);

  parallel: for (;;) { // see ParallelArrayMap() to explain why for(;;) etc
    if (ForceSequential())
      break parallel;
    if (!TRY_PARALLEL(m))
      break parallel;

    var chunks = ComputeNumChunks(length);
    var numSlices = ParallelSlices();
    if (chunks < numSlices)
      break parallel;

    var info = ComputeAllSliceBounds(chunks, numSlices);
    var subreductions = DenseArray(numSlices);
    ParallelDo(reduceSlice, CheckParallel(m));
    var acc = subreductions[0];
    for (var i = 1; i < numSlices; i++)
      acc = f(acc, subreductions[i]);
    return acc;
  }

  // Sequential fallback:
  var acc = self.get(0);
  for (var i = 1; i < length; i++)
    acc = f(acc, self.get(i));
  return acc;

  function reduceSlice(id, n, warmup) {
    var chunkStart = info[SLICE_START(id)];
    var chunkPos = info[SLICE_POS(id)];
    var chunkEnd = info[SLICE_END(id)];

    // (*) This function is carefully designed so that the warmup
    // (which executes with chunkStart === chunkPos) will execute
    // all potential loads and stores. In particular, the warmup run
    // processes two chunks rather than one.  Moreover, it stores acc
    // into subreductions and then loads it again ensure that the load
    // is executed during the warmup, as it will certainly be run
    // during subsequent runs.

    if (warmup && chunkEnd > chunkPos + 2)
      chunkEnd = chunkPos + 2;

    if (chunkStart === chunkPos) {
      var indexPos = chunkStart << CHUNK_SHIFT;
      var acc = reduceChunk(self.get(indexPos), indexPos + 1, indexPos + CHUNK_SIZE);

      UnsafeSetElement(subreductions, id, acc, // see (*) above
                       info, SLICE_POS(id), ++chunkPos);
    }

    var acc = subreductions[id]; // see (*) above

    while (chunkPos < chunkEnd) {
      var indexPos = chunkPos << CHUNK_SHIFT;
      acc = reduceChunk(acc, indexPos, indexPos + CHUNK_SIZE);
      UnsafeSetElement(subreductions, id, acc,
                       info, SLICE_POS(id), ++chunkPos);
    }
  }

  function reduceChunk(acc, from, to) {
    to = IntMin(to, length);
    for (var i = from; i < to; i++)
      acc = f(acc, self.get(i));
    return acc;
  }
}

function ParallelArrayScan(f, m) {
  var self = this;
  var length = self.shape[0];

  if (length === 0)
    ThrowError(JSMSG_PAR_ARRAY_REDUCE_EMPTY);

  var buffer = DenseArray(length);

  parallel: for (;;) { // see ParallelArrayMap() to explain why for(;;) etc
    if (ForceSequential())
      break parallel;
    if (!TRY_PARALLEL(m))
      break parallel;

    var chunks = ComputeNumChunks(length);
    var numSlices = ParallelSlices();
    if (chunks < numSlices)
      break parallel;
    var info = ComputeAllSliceBounds(chunks, numSlices);

    // Scan slices individually (see comment on phase1()).
    ParallelDo(phase1, CheckParallel(m));

    // Compute intermediates array (see comment on phase2()).
    var intermediates = [];
    var acc = intermediates[0] = buffer[finalElement(0)];
    for (var i = 1; i < numSlices - 1; i++)
      acc = intermediates[i] = f(acc, buffer[finalElement(i)]);

    // Reset the current position information for each slice, but
    // convert from chunks to indicies (see comment on phase2()).
    for (var i = 0; i < numSlices; i++) {
      info[SLICE_POS(i)] = info[SLICE_START(i)] << CHUNK_SHIFT;
      info[SLICE_END(i)] = info[SLICE_END(i)] << CHUNK_SHIFT;
    }
    info[SLICE_END(numSlices - 1)] = IntMin(info[SLICE_END(numSlices - 1)], length);

    // Complete each slice using intermediates array (see comment on phase2()).
    ParallelDo(phase2, CheckParallel(m));
    return NewParallelArray(ParallelArrayView, [length], buffer, 0);
  }

  // Sequential fallback:
  scan(self.get(0), 0, length);
  return NewParallelArray(ParallelArrayView, [length], buffer, 0);

  function scan(acc, start, end) {
    UnsafeSetElement(buffer, start, acc);
    for (var i = start + 1; i < end; i++) {
      acc = f(acc, self.get(i));
      UnsafeSetElement(buffer, i, acc);
    }
    return acc;
  }

  function phase1(id, n, warmup) {
    // In phase 1, we divide the source array into n slices and
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
    var chunkStart = info[SLICE_START(id)];
    var chunkPos = info[SLICE_POS(id)];
    var chunkEnd = info[SLICE_END(id)];

    if (warmup && chunkEnd > chunkPos + 2)
      chunkEnd = chunkPos + 2;

    if (chunkPos == chunkStart) {
      // For the first chunk, the accumulator begins as the value in
      // the input at the start of the chunk.
      var indexStart = chunkPos << CHUNK_SHIFT;
      var indexEnd = IntMin(indexStart + CHUNK_SIZE, length);
      scan(self.get(indexStart), indexStart, indexEnd);
      UnsafeSetElement(info, SLICE_POS(id), ++chunkPos);
    }

    while (chunkPos < chunkEnd) {
      // For each subsequent chunk, the accumulator begins as the
      // combination of the final value of prev chunk and the value in
      // the input at the start of this chunk.  Note that this loop is
      // written as simple as possible, at the cost of an extra read
      // from the buffer per iteration.
      var indexStart = chunkPos << CHUNK_SHIFT;
      var indexEnd = IntMin(indexStart + CHUNK_SIZE, length);
      var acc = f(buffer[indexStart - 1], self.get(indexStart));
      scan(acc, indexStart, indexEnd);
      UnsafeSetElement(info, SLICE_POS(id), ++chunkPos);
    }
  }

  function finalElement(id) {
    // Computes the index of the final element computed by the slice |id|.
    var chunkEnd = info[SLICE_END(id)]; // last chunk written by |id| is endChunk - 1
    var indexStart = IntMin(chunkEnd << CHUNK_SHIFT, length);
    return indexStart - 1;
  }

  function phase2(id, n, warmup) {
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
    // Phase 2 combines the results of phase1 with the intermediates
    // array to produce the final scan results.  The idea is to
    // reiterate over each element S[i] in the slice |id|, which
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
    // SUBTLE: Because we are mutating |buffer| in place, we have to
    // be very careful about bailouts!  We cannot checkpoint a chunk
    // at a time as we do elsewhere because that assumes it is safe to
    // replay the portion of a chunk which was already processed.
    // Therefore, in this phase, we track the current position at an
    // index granularity, although this requires two memory writes per
    // index.

    if (id == 0)
      return; // No work to do for the 0th slice.

    var indexPos = info[SLICE_POS(id)];
    var indexEnd = info[SLICE_END(id)];

    if (warmup)
      indexEnd = IntMin(indexEnd, indexPos + CHUNK_SIZE);

    var intermediate = intermediates[id - 1];
    for (; indexPos < indexEnd; indexPos++)
      UnsafeSetElement(buffer, indexPos, f(intermediate, buffer[indexPos]),
                       info, SLICE_POS(id), indexPos + 1);
  }
}

function ParallelArrayScatter(targets, zero, f, length, m) {

  var self = this;

  if (length === undefined)
    length = self.shape[0];

  // The Divide-Scatter-Vector strategy:
  // 1. Slice |targets| array of indices ("scatter-vector") into N
  //    parts.
  // 2. Each of the N threads prepares an output buffer and a
  //    write-log.
  // 3. Each thread scatters according to one of the N parts into its
  //    own output buffer, tracking written indices in the write-log
  //    and resolving any resulting local collisions in parallel.
  // 4. Merge the parts (either in parallel or sequentially), using
  //    the write-logs as both the basis for finding merge-inputs and
  //    for detecting collisions.

  // The Divide-Output-Range strategy:
  // 1. Slice the range of indices [0..|length|-1] into N parts.
  //    Allocate a single shared output buffer of length |length|.
  // 2. Each of the N threads scans (the entirety of) the |targets|
  //    array, seeking occurrences of indices from that thread's part
  //    of the range, and writing the results into the shared output
  //    buffer.
  // 3. Since each thread has its own portion of the output range,
  //    every collision that occurs can be handled thread-locally.

  // SO:
  //
  // If |targets.length| >> |length|, Divide-Scatter-Vector seems like
  // a clear win over Divide-Output-Range, since for the latter, the
  // expense of redundantly scanning the |targets| will diminish the
  // gain from processing |length| in parallel, while for the former,
  // the total expense of building separate output buffers and the
  // merging post-process is small compared to the gain from
  // processing |targets| in parallel.
  //
  // If |targets.length| << |length|, then Divide-Output-Range seems
  // like it *could* win over Divide-Scatter-Vector.  (But when is
  // |targets.length| << |length| or even |targets.length| < |length|?
  // Seems like an odd situation and an uncommon case at best.)
  //
  // The unanswered question is which strategy performs better when
  // |targets.length| approximately equals |length|, especially for
  // special cases like collision-free scatters and permutations.

  if (targets.length >>> 0 !== targets.length)
    ThrowError(JSMSG_BAD_ARRAY_LENGTH, "");

  var targetsLength = IntMin(targets.length, self.length);

  if (length && length >>> 0 !== length)
    ThrowError(JSMSG_BAD_ARRAY_LENGTH, "");

  parallel: for (;;) { // see ParallelArrayMap() to explain why for(;;) etc
    if (ForceSequential())
      break parallel;
    if (!TRY_PARALLEL(m))
      break parallel;

    if (forceDivideScatterVector())
      return parDivideScatterVector();
    else if (forceDivideOutputRange())
      return parDivideOutputRange();
    else if (f === undefined && targetsLength < length)
      return parDivideOutputRange();
    return parDivideScatterVector();
  }

  // Sequential fallback:
  return seq();

  function forceDivideScatterVector() {
    return m && m.strategy && m.strategy == "divide-scatter-vector";
  }

  function forceDivideOutputRange() {
    return m && m.strategy && m.strategy == "divide-output-range";
    return f(elem1, elem2);
  }

  function collide(elem1, elem2) {
    if (f === undefined)
      ThrowError(JSMSG_PAR_ARRAY_SCATTER_CONFLICT);

    return f(elem1, elem2);
  }


  function parDivideOutputRange() {
    var chunks = ComputeNumChunks(targetsLength);
    var numSlices = ParallelSlices();
    var checkpoints = DenseArray(numSlices);
    for (var i = 0; i < numSlices; i++)
      checkpoints[i] = 0;

    var buffer = DenseArray(length);
    var conflicts = DenseArray(length);

    for (var i = 0; i < length; i++)
      buffer[i] = zero;

    ParallelDo(fill, CheckParallel(m));
    return NewParallelArray(ParallelArrayView, [length], buffer, 0);

    function fill(id, n, warmup) {
      var indexPos = checkpoints[id];
      var indexEnd = targetsLength;
      if (warmup)
        indexEnd = IntMin(indexEnd, indexPos + CHUNK_SIZE);

      // Range in the output for which we are responsible:
      var [outputStart, outputEnd] = ComputeSliceBounds(length, id, numSlices);

      for (; indexPos < indexEnd; indexPos++) {
        var x = self.get(indexPos);
        var t = targets[indexPos];
        checkTarget(t);
        if (t < outputStart || t >= outputEnd)
          continue;
        if (conflicts[t])
          x = collide(x, buffer[t]);
        UnsafeSetElement(buffer, t, x,
                         conflicts, t, true,
                         checkpoints, id, indexPos + 1);
      }
    }
  }

  function parDivideScatterVector() {
    // Subtle: because we will be mutating the localbuffers and
    // conflict arrays in place, we can never replay an entry in the
    // target array for fear of inducing a conflict where none existed
    // before.  Therefore, we must proceed not by chunks but rather by
    // individual indices,
    var numSlices = ParallelSlices();
    var info = ComputeAllSliceBounds(targetsLength, numSlices);

    var localbuffers = DenseArray(numSlices);
    for (var i = 0; i < numSlices; i++)
        localbuffers[i] = DenseArray(length);
    var localconflicts = DenseArray(numSlices);
    for (var i = 0; i < numSlices; i++)
        localconflicts[i] = DenseArray(length);

    // Initialize the 0th buffer, which will become the output.  For
    // the other buffers, we track which parts have been written to
    // using the conflict buffer so they do not need to be
    // initialized.
    var outputbuffer = localbuffers[0];
    for (var i = 0; i < length; i++)
      outputbuffer[i] = zero;

    ParallelDo(fill, CheckParallel(m));
    mergeBuffers();
    return NewParallelArray(ParallelArrayView, [length], outputbuffer, 0);

    function fill(id, n, warmup) {
      var indexPos = info[SLICE_POS(id)];
      var indexEnd = info[SLICE_END(id)];
      if (warmup)
        indexEnd = IntMin(indexEnd, indexPos + CHUNK_SIZE);

      var localbuffer = localbuffers[id];
      var conflicts = localconflicts[id];
      while (indexPos < indexEnd) {
        var x = self.get(indexPos);
        var t = targets[indexPos];
        checkTarget(t);
        if (conflicts[t])
          x = collide(x, localbuffer[t]);
        UnsafeSetElement(localbuffer, t, x,
                         conflicts, t, true,
                         info, SLICE_POS(id), ++indexPos);
      }
    }

    function mergeBuffers() {
      // Merge buffers 1..N into buffer 0.  In principle, we could
      // parallelize the merge work as well.  But for this first cut,
      // just do the merge sequentially.
      var buffer = localbuffers[0];
      var conflicts = localconflicts[0];
      for (var i = 1; i < numSlices; i++) {
        var otherbuffer = localbuffers[i];
        var otherconflicts = localconflicts[i];
        for (var j = 0; j < length; j++) {
          if (otherconflicts[j]) {
            if (conflicts[j]) {
              buffer[j] = collide(otherbuffer[j], buffer[j]);
            } else {
              buffer[j] = otherbuffer[j];
              conflicts[j] = true;
            }
          }
        }
      }
    }
  }

  function seq() {
    var buffer = DenseArray(length);
    var conflicts = DenseArray(length);

    for (var i = 0; i < length; i++)
      buffer[i] = zero;

    for (var i = 0; i < targetsLength; i++) {
      var x = self.get(i);
      var t = targets[i];
      checkTarget(t);
      if (conflicts[t])
        x = collide(x, buffer[t]);

      UnsafeSetElement(buffer, t, x,
                       conflicts, t, true);
    }

    return NewParallelArray(ParallelArrayView, [length], buffer, 0);
  }

  function checkTarget(t) {
      if ((t | 0) !== t)
        ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, ".prototype.scatter");

      if (t >= length)
        ThrowError(JSMSG_PAR_ARRAY_SCATTER_BOUNDS);
  }
}

function ParallelArrayFilter(func, m) {
  var self = this;
  var length = self.shape[0];

  parallel: for (;;) { // see ParallelArrayMap() to explain why for(;;) etc
    if (ForceSequential())
      break parallel;
    if (!TRY_PARALLEL(m))
      break parallel;

    var chunks = ComputeNumChunks(length);
    var numSlices = ParallelSlices();
    if (chunks < numSlices * 2)
      break parallel;

    var info = ComputeAllSliceBounds(chunks, numSlices);

    // Step 1.  Compute which items from each slice of the result
    // buffer should be preserved.  When we're done, we have an array
    // |survivors| containing a bitset for each chunk, indicating
    // which members of the chunk survived.  We also keep an array
    // |counts| containing the total number of items that are being
    // preserved from within one slice.
    var counts = DenseArray(numSlices);
    for (var i = 0; i < numSlices; i++)
      counts[i] = 0;
    var survivors = DenseArray(chunks);
    ParallelDo(findSurvivorsInSlice, CheckParallel(m));

    // Step 2. Compress the slices into one contiguous set.
    var count = 0;
    for (var i = 0; i < numSlices; i++)
      count += counts[i];
    var buffer = DenseArray(count);
    if (count > 0)
      ParallelDo(copySurvivorsInSlice, CheckParallel(m));

    return NewParallelArray(ParallelArrayView, [count], buffer, 0);
  }

  // Sequential fallback:
  var buffer = [], count = 0;
  for (var i = 0; i < length; i++) {
    var elem = self.get(i);
    if (func(elem, i, self))
      buffer[count++] = elem;
  }
  return NewParallelArray(ParallelArrayView, [count], buffer, 0);

  function findSurvivorsInSlice(id, n, warmup) {
    // As described above, our goal is to determine which items we
    // will preserve from a given slice.  We do this one chunk at a
    // time. When we finish a chunk, we record our current count and
    // the next chunk id, lest we should bail.

    var chunkPos = info[SLICE_POS(id)];
    var chunkEnd = info[SLICE_END(id)];

    if (warmup && chunkEnd > chunkPos)
      chunkEnd = chunkPos + 1;

    var count = counts[id];
    while (chunkPos < chunkEnd) {
      var indexStart = chunkPos << CHUNK_SHIFT;
      var indexEnd = IntMin(indexStart + CHUNK_SIZE, length);
      var chunkBits = 0;

      for (var bit = 0; indexStart + bit < indexEnd; bit++) {
        var keep = !!func(self.get(indexStart + bit), indexStart + bit, self);
        chunkBits |= keep << bit;
        count += keep;
      }

      UnsafeSetElement(survivors, chunkPos, chunkBits,
                       counts, id, count,
                       info, SLICE_POS(id), ++chunkPos);
    }
  }

  function copySurvivorsInSlice(id, n, warmup) {
    // Copies the survivors from this slice into the correct position.
    // Note that this is an idempotent operation that does not invoke
    // user code.  Therefore, we don't expect bailouts and make an
    // effort to proceed chunk by chunk or avoid duplicating work.

    // During warmup, we only execute with id 0.  This would fail to
    // execute the loop below.  Therefore, during warmup, we
    // substitute 1 for the id.
    if (warmup && id == 0 && n != 1)
      id = 1;

    // Total up the items preserved by previous slices.
    var count = 0;
    if (id > 0) { // FIXME(#819219)---work around a bug in Ion's range checks
      for (var i = 0; i < id; i++)
        count += counts[i];
    }

    // Compute the final index we expect to write.
    var total = count + counts[id];
    if (count == total)
      return;

    // Iterate over the chunks assigned to us. Read the bitset for
    // each chunk.  Copy values where a 1 appears until we have
    // written all the values that we expect to.  We can just iterate
    // from 0...CHUNK_SIZE without fear of a truncated final chunk
    // because we are already checking for when count==total.
    var chunkStart = info[SLICE_START(id)];
    var chunkEnd = info[SLICE_END(id)];
    for (var chunk = chunkStart; chunk < chunkEnd; chunk++) {
      var chunkBits = survivors[chunk];
      var indexStart = chunk << CHUNK_SHIFT;
      for (var i = 0; i < CHUNK_SIZE; i++) {
        if (chunkBits & (1 << i)) {
          UnsafeSetElement(buffer, count++, self.get(indexStart + i));
          if (count == total)
            break;
        }
      }
    }
  }
}

function ParallelArrayPartition(amount) {
  if (amount >>> 0 !== amount)
    ThrowError(JSMSG_BAD_ARRAY_LENGTH, ""); // XXX

  var length = this.shape[0];
  var partitions = (length / amount) | 0;

  if (partitions * amount !== length)
    ThrowError(JSMSG_BAD_ARRAY_LENGTH, ""); // XXX

  var shape = [partitions, amount];
  for (var i = 1; i < this.shape.length; i++)
    shape.push(this.shape[i]);
  return NewParallelArray(ParallelArrayView, shape, this.buffer, this.offset);
}

function ParallelArrayFlatten() {
  if (this.shape.length < 2)
    ThrowError(JSMSG_BAD_ARRAY_LENGTH, ""); // XXX

  var shape = [this.shape[0] * this.shape[1]];
  for (var i = 2; i < this.shape.length; i++)
    shape.push(this.shape[i]);
  return NewParallelArray(ParallelArrayView, shape, this.buffer, this.offset);
}

//
// Accessors and utilities.
//

function ParallelArrayGet1(i) {
  if (i === undefined)
    return undefined;
  return this.buffer[this.offset + i];
}

function ParallelArrayGet2(x, y) {
  var xw = this.shape[0];
  var yw = this.shape[1];
  if (x === undefined)
    return undefined;
  if (x >= xw)
    return undefined;
  if (y === undefined)
    return NewParallelArray(ParallelArrayView, [yw], this.buffer, this.offset + x*yw);
  if (y >= yw)
    return undefined;
  var offset = y + x*yw;
  return this.buffer[this.offset + offset];
}

function ParallelArrayGet3(x, y, z) {
  var xw = this.shape[0];
  var yw = this.shape[1];
  var zw = this.shape[2];
  if (x === undefined)
    return undefined;
  if (x >= xw)
    return undefined;
  if (y === undefined)
    return NewParallelArray(ParallelArrayView, [yw, zw], this.buffer, this.offset + x*yw*zw);
  if (y >= yw)
    return undefined;
  if (z === undefined)
    return NewParallelArray(ParallelArrayView, [zw], this.buffer, this.offset + y*zw + x*yw*zw);
  if (z >= zw)
    return undefined;
  var offset = z + y*zw + x*yw*zw;
  return this.buffer[this.offset + offset];
}

function ParallelArrayGetN(...coords) {
  if (coords.length == 0)
    return undefined;

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
      return undefined;
    offset += coords[i] * products[sdimensionality - i - 1];
  }

  if (cdimensionality < sdimensionality) {
    var shape = this.shape.slice(cdimensionality);
    return NewParallelArray(ParallelArrayView, shape, this.buffer, offset);
  }
  return this.buffer[offset];
}

function ParallelArrayLength() {
  return this.shape[0];
}

function ParallelArrayToString() {
  var l = this.shape[0];
  if (l == 0)
    return "";

  var open, close;
  if (this.shape.length > 1) {
    open = "<"; close = ">";
  } else {
    open = close = "";
  }

  var result = "";
  for (var i = 0; i < l - 1; i++) {
    result += open + this.get(i).toString() + close;
    result += ",";
  }
  result += open + this.get(l-1).toString() + close;
  return result;
}

function CheckParallel(m) {
  if (!m)
    return null;

  return function(bailouts) {
    if (!("expect" in m) || m.expect === "any") {
      return; // Ignore result when unspecified or unimportant.
    }

    var result;
    if (bailouts === 0)
      result = "success";
    else if (bailouts === global.Infinity)
      result = "disqualified";
    else
      result = "bailout";

    if (m.expect === "mixed") {
      if (result !== "success" && result !== "bailout")
        ThrowError(JSMSG_PAR_ARRAY_MODE_FAILURE, m.expect, result);
    } else if (result !== m.expect) {
      ThrowError(JSMSG_PAR_ARRAY_MODE_FAILURE, m.expect, result);
    }
  };
}

// Mark the main operations as clone-at-callsite for better precision.
SetFunctionFlags(ParallelArrayConstruct0, { cloneAtCallsite: true });
SetFunctionFlags(ParallelArrayConstruct1, { cloneAtCallsite: true });
SetFunctionFlags(ParallelArrayConstruct2, { cloneAtCallsite: true });
SetFunctionFlags(ParallelArrayConstruct3, { cloneAtCallsite: true });
SetFunctionFlags(ParallelArrayView,       { cloneAtCallsite: true });
SetFunctionFlags(ParallelArrayBuild,      { cloneAtCallsite: true });
SetFunctionFlags(ParallelArrayMap,        { cloneAtCallsite: true });
SetFunctionFlags(ParallelArrayReduce,     { cloneAtCallsite: true });
SetFunctionFlags(ParallelArrayScan,       { cloneAtCallsite: true });
SetFunctionFlags(ParallelArrayScatter,    { cloneAtCallsite: true });
SetFunctionFlags(ParallelArrayFilter,     { cloneAtCallsite: true });

// Mark the common getters as clone-at-callsite.
SetFunctionFlags(ParallelArrayGet1,       { cloneAtCallsite: true });
SetFunctionFlags(ParallelArrayGet2,       { cloneAtCallsite: true });
SetFunctionFlags(ParallelArrayGet3,       { cloneAtCallsite: true });

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
