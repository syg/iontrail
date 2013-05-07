/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// FIXME(bug 844882): Parallel array properties should not be exposed.

// The mode asserts options object.
#define TRY_PARALLEL(MODE) \
  ((!MODE || MODE.mode === "par"))
#define ASSERT_SEQUENTIAL_IS_OK(MODE) \
  do { if (MODE) AssertSequentialIsOK(MODE) } while(false)

// Slice array: see ComputeAllSliceBounds()
#define SLICE_INFO(START, END) START, END, START, 0
#define SLICE_START(ID) ((ID << 2) + 0)
#define SLICE_END(ID)   ((ID << 2) + 1)
#define SLICE_POS(ID)   ((ID << 2) + 2)

// How many items at a time do we do recomp. for parallel execution.
// Note that filter currently assumes that this is no greater than 32
// in order to make use of a bitset.
#define CHUNK_SHIFT 5
#define CHUNK_SIZE 32

// Safe versions of ARRAY.push(ELEMENT)
#define ARRAY_PUSH(ARRAY, ELEMENT) \
  callFunction(std_Array_push, ARRAY, ELEMENT);
#define ARRAY_SLICE(ARRAY, ELEMENT) \
  callFunction(std_Array_slice, ARRAY, ELEMENT);

/**
 * Determine the number of chunks of size CHUNK_SIZE;
 * note that the final chunk may be smaller than CHUNK_SIZE.
 */
function ComputeNumChunks(length) {
  var chunks = length >>> CHUNK_SHIFT;
  if (chunks << CHUNK_SHIFT === length)
    return chunks;
  return chunks + 1;
}

/**
 * Computes the bounds for slice |sliceIndex| of |numItems| items,
 * assuming |numSlices| total slices. If numItems is not evenly
 * divisible by numSlices, then the final thread may have a bit of
 * extra work.
 */
function ComputeSliceBounds(numItems, sliceIndex, numSlices) {
  var sliceWidth = (numItems / numSlices) | 0;
  var startIndex = sliceWidth * sliceIndex;
  var endIndex = sliceIndex === numSlices - 1 ? numItems : sliceWidth * (sliceIndex + 1);
  return [startIndex, endIndex];
}

/**
 * Divides |numItems| items amongst |numSlices| slices. The result
 * is an array containing multiple values per slice: the start
 * index, end index, current position, and some padding. The
 * current position is initially the same as the start index. To
 * access the values for a particular slice, use the macros
 * SLICE_START() and so forth.
 */
function ComputeAllSliceBounds(numItems, numSlices) {
  // FIXME(bug 844890): Use typed arrays here.
  var info = [];
  for (var i = 0; i < numSlices; i++) {
    var [start, end] = ComputeSliceBounds(numItems, i, numSlices);
    ARRAY_PUSH(info, SLICE_INFO(start, end));
  }
  return info;
}

/**
 * Compute the partial products in reverse order.
 * e.g., if the shape is [A,B,C,D], then the
 * array |products| will be [1,D,CD,BCD].
 */
function ComputeProducts(shape) {
  var product = 1;
  var products = [1];
  var sdimensionality = shape.length;
  for (var i = sdimensionality - 1; i > 0; i--) {
    product *= shape[i];
    ARRAY_PUSH(products, product);
  }
  return products;
}

/**
 * Given a shape and some index |index1d|, computes and returns an
 * array containing the N-dimensional index that maps to |index1d|.
 */
function ComputeIndices(shape, index1d) {

  var products = ComputeProducts(shape);
  var l = shape.length;

  var result = [];
  for (var i = 0; i < l; i++) {
    // Obtain product of all higher dimensions.
    // So if i == 0 and shape is [A,B,C,D], yields BCD.
    var stride = products[l - i - 1];

    // Compute how many steps of width stride we could take.
    var index = (index1d / stride) | 0;
    ARRAY_PUSH(result, index);

    // Adjust remaining indices for smaller dimensions.
    index1d -= (index * stride);
  }

  return result;
}

function StepIndices(shape, indices) {
  for (var i = shape.length - 1; i >= 0; i--) {
    var indexi = indices[i] + 1;
    if (indexi < shape[i]) {
      indices[i] = indexi;
      return;
    }
    indices[i] = 0;
  }
}

// Constructor
//
// We split the 3 construction cases so that we don't case on arguments.

/**
 * This is the function invoked for |new ParallelArray()|
 */
function ParallelArrayConstructEmpty() {
  this.buffer = [];
  this.offset = 0;
  this.shape = [0];
  this.get = ParallelArrayGet1;
}

/**
 * This is the function invoked for |new ParallelArray(array)|.
 * It copies the data from its array-like argument |array|.
 */
function ParallelArrayConstructFromArray(buffer) {
  var buffer = ToObject(buffer);
  var length = buffer.length >>> 0;
  if (length !== buffer.length)
    ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");

  var buffer1 = [];
  for (var i = 0; i < length; i++)
    ARRAY_PUSH(buffer1, buffer[i]);

  this.buffer = buffer1;
  this.offset = 0;
  this.shape = [length];
  this.get = ParallelArrayGet1;
}

/**
 * Wrapper around |ParallelArrayConstructFromComprehension()| for the
 * case where 2 arguments are supplied. This is typically what users will
 * invoke. We provide an explicit two-argument version rather than
 * relying on JS's semantics for absent arguments because it simplifies
 * the ion code that does inlining of PA constructors.
 */
function ParallelArrayConstructFromFunction(shape, func) {
  return ParallelArrayConstructFromComprehension(this, shape, func, undefined);
}

/**
 * Wrapper around |ParallelArrayConstructFromComprehension()| for the
 * case where 3 arguments are supplied.
 */
function ParallelArrayConstructFromFunctionMode(shape, func, mode) {
  return ParallelArrayConstructFromComprehension(this, shape, func, mode);
}

/**
 * "Comprehension form": This is the function invoked for |new
 * ParallelArray(dim, fn)|. If |dim| is a number, then it creates a
 * new 1-dimensional parallel array with shape |[dim]| where index |i|
 * is equal to |fn(i)|. If |dim| is a vector, then it creates a new
 * N-dimensional parallel array where index |a, b, ... z| is equal to
 * |fn(a, b, ...z)|.
 *
 * The final |mode| argument is an internal argument used only
 * during our unit-testing.
 */
function ParallelArrayConstructFromComprehension(self, shape, func, mode) {
  // FIXME(bug 844887): Check |IsCallable(func)|

  if (typeof shape === "number") {
    var length = shape >>> 0;
    if (length !== shape)
      ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");
    ParallelArrayBuild(self, [length], func, mode);
  } else if (!shape || typeof shape.length !== "number") {
    ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");
  } else {
    var shape1 = [];
    for (var i = 0, l = shape.length; i < l; i++) {
      var s0 = shape[i];
      var s1 = s0 >>> 0;
      if (s1 !== s0)
        ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");
      ARRAY_PUSH(shape1, s1);
    }
    ParallelArrayBuild(self, shape1, func, mode);
  }
}

/**
 * Internal function used when constructing new parallel arrays. The
 * NewParallelArray() intrinsic takes a ctor function which it invokes
 * with the given shape, buffer, offset. The |this| parameter will be
 * the newly constructed parallel array.
 */
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

  // Due to inlining of NewParallelArray, the return type of this function
  // gets recorded as the return type of NewParallelArray at inlined sites, so
  // we must take care to return the same thing.
  return this;
}

function ProductOfArrayRange(shape, start, limit) {
  var length = 1;
  for (var i = start; i < limit; i++)
    length *= shape[i];
  return length;
}

/**
 * Helper for the comprehension form. Constructs an N-dimensional
 * array where |N == shape.length|. |shape| must be an array of
 * integers. The data for any given index vector |i| is determined by
 * |func(...i)|.
 */
function ParallelArrayBuild(self, shape, func, mode) {
  self.offset = 0;
  self.shape = shape;

  var length;
  var xDimension, yDimension, zDimension;
  var computefunc;

  switch (shape.length) {
  case 1:
    length = shape[0];
    self.get = ParallelArrayGet1;
    computefunc = fill1;
    break;
  case 2:
    xDimension = shape[0];
    yDimension = shape[1];
    length = xDimension * yDimension;
    self.get = ParallelArrayGet2;
    computefunc = fill2;
    break;
  case 3:
    xDimension = shape[0];
    yDimension = shape[1];
    zDimension = shape[2];
    length = xDimension * yDimension * zDimension;
    self.get = ParallelArrayGet3;
    computefunc = fill3;
    break;
  default:
    length = ProductOfArrayRange(shape, 0, shape.length);
    self.get = ParallelArrayGetN;
    computefunc = fillN;
    break;
  }

  var buffer = self.buffer = NewDenseArray(length);

  parallel: for (;;) {
    // Avoid parallel compilation if we are already nested in another
    // parallel section or the user told us not to parallelize. The
    // use of a for (;;) loop is working around some ion limitations:
    //
    // - Breaking out of named blocks does not currently work (bug 684384);
    // - Unreachable Code Elim. can't properly handle if (a && b) (bug 669796)
    if (ShouldForceSequential())
      break parallel;
    if (!TRY_PARALLEL(mode))
      break parallel;
    if (computefunc === fillN)
      break parallel;

    var chunks = ComputeNumChunks(length);
    var numSlices = ParallelSlices();
    var info = ComputeAllSliceBounds(chunks, numSlices);
    ParallelDo(constructSlice, CheckParallel(mode));
    return;
  }

  // Sequential fallback:
  ASSERT_SEQUENTIAL_IS_OK(mode);
  computefunc(0, length);
  return;

  function constructSlice(sliceId, numSlices, warmup) {
    var chunkPos = info[SLICE_POS(sliceId)];
    var chunkEnd = info[SLICE_END(sliceId)];

    if (warmup && chunkEnd > chunkPos)
      chunkEnd = chunkPos + 1;

    while (chunkPos < chunkEnd) {
      var indexStart = chunkPos << CHUNK_SHIFT;
      var indexEnd = std_Math_min(indexStart + CHUNK_SIZE, length);
      computefunc(indexStart, indexEnd);
      UnsafeSetElement(info, SLICE_POS(sliceId), ++chunkPos);
    }
  }

  function fill1(indexStart, indexEnd) {
    for (var i = indexStart; i < indexEnd; i++)
      UnsafeSetElement(buffer, i, func(i));
  }

  function fill2(indexStart, indexEnd) {
    var x = (indexStart / yDimension) | 0;
    var y = indexStart - x * yDimension;
    for (var i = indexStart; i < indexEnd; i++) {
      UnsafeSetElement(buffer, i, func(x, y));
      if (++y == yDimension) {
        y = 0;
        ++x;
      }
    }
  }

  function fill3(indexStart, indexEnd) {
    var x = (indexStart / (yDimension * zDimension)) | 0;
    var r = indexStart - x * yDimension * zDimension;
    var y = (r / zDimension) | 0;
    var z = r - y * zDimension;
    for (var i = indexStart; i < indexEnd; i++) {
      UnsafeSetElement(buffer, i, func(x, y, z));
      if (++z == zDimension) {
        z = 0;
        if (++y == yDimension) {
          y = 0;
          ++x;
        }
      }
    }
  }

  function fillN(indexStart, indexEnd) {
    var indices = ComputeIndices(shape, indexStart);
    for (var i = indexStart; i < indexEnd; i++) {
      var result = callFunction(std_Function_apply, func, null, indices);
      UnsafeSetElement(buffer, i, result);
      StepIndices(shape, indices);
    }
  }
}

/**
 * Creates a new parallel array by applying |func(e, i, self)| for each
 * element |e| with index |i|. Note that
 * this always operates on the outermost dimension only.
 */
function ParallelArrayMap(func, mode) {
  // FIXME(bug 844887): Check |this instanceof ParallelArray|
  // FIXME(bug 844887): Check |IsCallable(func)|

  var self = this;
  var length = self.shape[0];
  var buffer = NewDenseArray(length);

  parallel: for (;;) { // see ParallelArrayBuild() to explain why for(;;) etc
    if (ShouldForceSequential())
      break parallel;
    if (!TRY_PARALLEL(mode))
      break parallel;

    var chunks = ComputeNumChunks(length);
    var numSlices = ParallelSlices();
    var info = ComputeAllSliceBounds(chunks, numSlices);
    ParallelDo(mapSlice, CheckParallel(mode));
    return NewParallelArray(ParallelArrayView, [length], buffer, 0);
  }

  // Sequential fallback:
  ASSERT_SEQUENTIAL_IS_OK(mode);
  for (var i = 0; i < length; i++) {
    // Note: Unlike JS arrays, parallel arrays cannot have holes.
    var v = func(self.get(i), i, self);
    UnsafeSetElement(buffer, i, v);
  }
  return NewParallelArray(ParallelArrayView, [length], buffer, 0);

  function mapSlice(sliceId, numSlices, warmup) {
    var chunkPos = info[SLICE_POS(sliceId)];
    var chunkEnd = info[SLICE_END(sliceId)];

    if (warmup && chunkEnd > chunkPos + 1)
      chunkEnd = chunkPos + 1;

    while (chunkPos < chunkEnd) {
      var indexStart = chunkPos << CHUNK_SHIFT;
      var indexEnd = std_Math_min(indexStart + CHUNK_SIZE, length);

      for (var i = indexStart; i < indexEnd; i++)
        UnsafeSetElement(buffer, i, func(self.get(i), i, self));

      UnsafeSetElement(info, SLICE_POS(sliceId), ++chunkPos);
    }
  }
}

/**
 * Reduces the elements in a parallel array's outermost dimension
 * using the given reduction function.
 */
function ParallelArrayReduce(func, mode) {
  // FIXME(bug 844887): Check |this instanceof ParallelArray|
  // FIXME(bug 844887): Check |IsCallable(func)|

  var self = this;
  var length = self.shape[0];

  if (length === 0)
    ThrowError(JSMSG_PAR_ARRAY_REDUCE_EMPTY);

  parallel: for (;;) { // see ParallelArrayBuild() to explain why for(;;) etc
    if (ShouldForceSequential())
      break parallel;
    if (!TRY_PARALLEL(mode))
      break parallel;

    var chunks = ComputeNumChunks(length);
    var numSlices = ParallelSlices();
    if (chunks < numSlices)
      break parallel;

    var info = ComputeAllSliceBounds(chunks, numSlices);
    var subreductions = NewDenseArray(numSlices);
    ParallelDo(reduceSlice, CheckParallel(mode));
    var accumulator = subreductions[0];
    for (var i = 1; i < numSlices; i++)
      accumulator = func(accumulator, subreductions[i]);
    return accumulator;
  }

  // Sequential fallback:
  ASSERT_SEQUENTIAL_IS_OK(mode);
  var accumulator = self.get(0);
  for (var i = 1; i < length; i++)
    accumulator = func(accumulator, self.get(i));
  return accumulator;

  function reduceSlice(sliceId, numSlices, warmup) {
    var chunkStart = info[SLICE_START(sliceId)];
    var chunkPos = info[SLICE_POS(sliceId)];
    var chunkEnd = info[SLICE_END(sliceId)];

    // (*) This function is carefully designed so that the warmup
    // (which executes with chunkStart === chunkPos) will execute all
    // potential loads and stores. In particular, the warmup run
    // processes two chunks rather than one. Moreover, it stores
    // accumulator into subreductions and then loads it again to
    // ensure that the load is executed during the warmup, as it will
    // certainly be executed during subsequent runs.

    if (warmup && chunkEnd > chunkPos + 2)
      chunkEnd = chunkPos + 2;

    if (chunkStart === chunkPos) {
      var indexPos = chunkStart << CHUNK_SHIFT;
      var accumulator = reduceChunk(self.buffer[self.offset+indexPos], indexPos + 1, indexPos + CHUNK_SIZE);

      UnsafeSetElement(subreductions, sliceId, accumulator, // see (*) above
                       info, SLICE_POS(sliceId), ++chunkPos);
    }

    var accumulator = subreductions[sliceId]; // see (*) above

    while (chunkPos < chunkEnd) {
      var indexPos = chunkPos << CHUNK_SHIFT;
      accumulator = reduceChunk(accumulator, indexPos, indexPos + CHUNK_SIZE);
      UnsafeSetElement(subreductions, sliceId, accumulator,
                       info, SLICE_POS(sliceId), ++chunkPos);
    }
  }

  function reduceChunk(accumulator, from, to) {
    to = std_Math_min(to, length);
    for (var i = from; i < to; i++)
      accumulator = func(accumulator, self.buffer[self.offset+i]);
    return accumulator;
  }
}

/**
 * |scan()| returns an array [s_0, ..., s_N] where
 * |s_i| is equal to the reduction (as per |reduce()|)
 * of elements |0..i|. This is the generalization
 * of partial sum.
 */
function ParallelArrayScan(func, mode) {
  // FIXME(bug 844887): Check |this instanceof ParallelArray|
  // FIXME(bug 844887): Check |IsCallable(func)|

  var self = this;
  var length = self.shape[0];

  if (length === 0)
    ThrowError(JSMSG_PAR_ARRAY_REDUCE_EMPTY);

  var buffer = NewDenseArray(length);

  parallel: for (;;) { // see ParallelArrayBuild() to explain why for(;;) etc
    if (ShouldForceSequential())
      break parallel;
    if (!TRY_PARALLEL(mode))
      break parallel;

    var chunks = ComputeNumChunks(length);
    var numSlices = ParallelSlices();
    if (chunks < numSlices)
      break parallel;
    var info = ComputeAllSliceBounds(chunks, numSlices);

    // Scan slices individually (see comment on phase1()).
    ParallelDo(phase1, CheckParallel(mode));

    // Compute intermediates array (see comment on phase2()).
    var intermediates = [];
    var accumulator = buffer[finalElement(0)];
    ARRAY_PUSH(intermediates, accumulator);
    for (var i = 1; i < numSlices - 1; i++) {
      accumulator = func(accumulator, buffer[finalElement(i)]);
      ARRAY_PUSH(intermediates, accumulator);
    }

    // Reset the current position information for each slice, but
    // convert from chunks to indices (see comment on phase2()).
    for (var i = 0; i < numSlices; i++) {
      info[SLICE_POS(i)] = info[SLICE_START(i)] << CHUNK_SHIFT;
      info[SLICE_END(i)] = info[SLICE_END(i)] << CHUNK_SHIFT;
    }
    info[SLICE_END(numSlices - 1)] = std_Math_min(info[SLICE_END(numSlices - 1)], length);

    // Complete each slice using intermediates array (see comment on phase2()).
    ParallelDo(phase2, CheckParallel(mode));
    return NewParallelArray(ParallelArrayView, [length], buffer, 0);
  }

  // Sequential fallback:
  ASSERT_SEQUENTIAL_IS_OK(mode);
  scan(self.get(0), 0, length);
  return NewParallelArray(ParallelArrayView, [length], buffer, 0);

  function scan(accumulator, start, end) {
    UnsafeSetElement(buffer, start, accumulator);
    for (var i = start + 1; i < end; i++) {
      accumulator = func(accumulator, self.get(i));
      UnsafeSetElement(buffer, i, accumulator);
    }
    return accumulator;
  }

  /**
   * In phase 1, we divide the source array into |numSlices| slices and
   * compute scan on each slice sequentially as if it were the entire
   * array. This function is responsible for computing one of those
   * slices.
   *
   * So, if we have an array [A,B,C,D,E,F,G,H,I], |numSlices == 3|,
   * and our function |func| is sum, then we would wind up computing a
   * result array like:
   *
   *     [A, A+B, A+B+C, D, D+E, D+E+F, G, G+H, G+H+I]
   *      ^~~~~~~~~~~~^  ^~~~~~~~~~~~^  ^~~~~~~~~~~~~^
   *      Slice 0        Slice 1        Slice 2
   *
   * Read on in phase2 to see what we do next!
   */
  function phase1(sliceId, numSlices, warmup) {
    var chunkStart = info[SLICE_START(sliceId)];
    var chunkPos = info[SLICE_POS(sliceId)];
    var chunkEnd = info[SLICE_END(sliceId)];

    if (warmup && chunkEnd > chunkPos + 2)
      chunkEnd = chunkPos + 2;

    if (chunkPos == chunkStart) {
      // For the first chunk, the accumulator begins as the value in
      // the input at the start of the chunk.
      var indexStart = chunkPos << CHUNK_SHIFT;
      var indexEnd = std_Math_min(indexStart + CHUNK_SIZE, length);
      scan(self.get(indexStart), indexStart, indexEnd);
      UnsafeSetElement(info, SLICE_POS(sliceId), ++chunkPos);
    }

    while (chunkPos < chunkEnd) {
      // For each subsequent chunk, the accumulator begins as the
      // combination of the final value of prev chunk and the value in
      // the input at the start of this chunk. Note that this loop is
      // written as simple as possible, at the cost of an extra read
      // from the buffer per iteration.
      var indexStart = chunkPos << CHUNK_SHIFT;
      var indexEnd = std_Math_min(indexStart + CHUNK_SIZE, length);
      var accumulator = func(buffer[indexStart - 1], self.get(indexStart));
      scan(accumulator, indexStart, indexEnd);
      UnsafeSetElement(info, SLICE_POS(sliceId), ++chunkPos);
    }
  }

  /**
   * Computes the index of the final element computed by the slice |sliceId|.
   */
  function finalElement(sliceId) {
    var chunkEnd = info[SLICE_END(sliceId)]; // last chunk written by |sliceId| is endChunk - 1
    var indexStart = std_Math_min(chunkEnd << CHUNK_SHIFT, length);
    return indexStart - 1;
  }

  /**
   * After computing the phase1 results, we compute an
   * |intermediates| array. |intermediates[i]| contains the result
   * of reducing the final value from each preceding slice j<i with
   * the final value of slice i. So, to continue our previous
   * example, the intermediates array would contain:
   *
   *   [A+B+C, (A+B+C)+(D+E+F), ((A+B+C)+(D+E+F))+(G+H+I)]
   *
   * Here I have used parenthesization to make clear the order of
   * evaluation in each case.
   *
   *   An aside: currently the intermediates array is computed
   *   sequentially. In principle, we could compute it in parallel,
   *   at the cost of doing duplicate work. This did not seem
   *   particularly advantageous to me, particularly as the number
   *   of slices is typically quite small (one per core), so I opted
   *   to just compute it sequentially.
   *
   * Phase 2 combines the results of phase1 with the intermediates
   * array to produce the final scan results. The idea is to
   * reiterate over each element S[i] in the slice |sliceId|, which
   * currently contains the result of reducing with S[0]...S[i]
   * (where S0 is the first thing in the slice), and combine that
   * with |intermediate[sliceId-1]|, which represents the result of
   * reducing everything in the input array prior to the slice.
   *
   * To continue with our example, in phase 1 we computed slice 1 to
   * be [D, D+E, D+E+F]. We will combine those results with
   * |intermediates[1-1]|, which is |A+B+C|, so that the final
   * result is [(A+B+C)+D, (A+B+C)+(D+E), (A+B+C)+(D+E+F)]. Again I
   * am using parentheses to clarify how these results were reduced.
   *
   * SUBTLE: Because we are mutating |buffer| in place, we have to
   * be very careful about bailouts!  We cannot checkpoint a chunk
   * at a time as we do elsewhere because that assumes it is safe to
   * replay the portion of a chunk which was already processed.
   * Therefore, in this phase, we track the current position at an
   * index granularity, although this requires two memory writes per
   * index.
   */
  function phase2(sliceId, numSlices, warmup) {
    if (sliceId == 0)
      return; // No work to do for the 0th slice.

    var indexPos = info[SLICE_POS(sliceId)];
    var indexEnd = info[SLICE_END(sliceId)];

    if (warmup)
      indexEnd = std_Math_min(indexEnd, indexPos + CHUNK_SIZE);

    var intermediate = intermediates[sliceId - 1];
    for (; indexPos < indexEnd; indexPos++) {
      UnsafeSetElement(buffer, indexPos, func(intermediate, buffer[indexPos]),
                       info, SLICE_POS(sliceId), indexPos + 1);
    }
  }
}

/**
 * |scatter()| redistributes the elements in the parallel array
 * into a new parallel array.
 *
 * - targets: The index targets[i] indicates where the ith element
 *   should appear in the result.
 *
 * - defaultValue: what value to use for indices in the output array that
 *   are never targeted.
 *
 * - conflictFunc: The conflict function. Used to resolve what
 *   happens if two indices i and j in the source array are targeted
 *   as the same destination (i.e., targets[i] == targets[j]), then
 *   the final result is determined by applying func(targets[i],
 *   targets[j]). If no conflict function is provided, it is an error
 *   if targets[i] == targets[j].
 *
 * - length: length of the output array (if not specified, uses the
 *   length of the input).
 *
 * - mode: internal debugging specification.
 */
function ParallelArrayScatter(targets, defaultValue, conflictFunc, length, mode) {
  // FIXME(bug 844887): Check |this instanceof ParallelArray|
  // FIXME(bug 844887): Check targets is array-like
  // FIXME(bug 844887): Check |IsCallable(conflictFunc)|

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
  // like it *could* win over Divide-Scatter-Vector. (But when is
  // |targets.length| << |length| or even |targets.length| < |length|?
  // Seems like an odd situation and an uncommon case at best.)
  //
  // The unanswered question is which strategy performs better when
  // |targets.length| approximately equals |length|, especially for
  // special cases like collision-free scatters and permutations.

  if (targets.length >>> 0 !== targets.length)
    ThrowError(JSMSG_BAD_ARRAY_LENGTH, ".prototype.scatter");

  var targetsLength = std_Math_min(targets.length, self.length);

  if (length >>> 0 !== length)
    ThrowError(JSMSG_BAD_ARRAY_LENGTH, ".prototype.scatter");

  parallel: for (;;) { // see ParallelArrayBuild() to explain why for(;;) etc
    if (ShouldForceSequential())
      break parallel;
    if (!TRY_PARALLEL(mode))
      break parallel;

    if (forceDivideScatterVector())
      return parDivideScatterVector();
    else if (forceDivideOutputRange())
      return parDivideOutputRange();
    else if (conflictFunc === undefined && targetsLength < length)
      return parDivideOutputRange();
    return parDivideScatterVector();
  }

  // Sequential fallback:
  ASSERT_SEQUENTIAL_IS_OK(mode);
  return seq();

  function forceDivideScatterVector() {
    return mode && mode.strategy && mode.strategy == "divide-scatter-vector";
  }

  function forceDivideOutputRange() {
    return mode && mode.strategy && mode.strategy == "divide-output-range";
  }

  function collide(elem1, elem2) {
    if (conflictFunc === undefined)
      ThrowError(JSMSG_PAR_ARRAY_SCATTER_CONFLICT);

    return conflictFunc(elem1, elem2);
  }


  function parDivideOutputRange() {
    var chunks = ComputeNumChunks(targetsLength);
    var numSlices = ParallelSlices();
    var checkpoints = NewDenseArray(numSlices);
    for (var i = 0; i < numSlices; i++)
      UnsafeSetElement(checkpoints, i, 0);

    var buffer = NewDenseArray(length);
    var conflicts = NewDenseArray(length);

    for (var i = 0; i < length; i++) {
      UnsafeSetElement(buffer, i, defaultValue);
      UnsafeSetElement(conflicts, i, false);
    }

    ParallelDo(fill, CheckParallel(mode));
    return NewParallelArray(ParallelArrayView, [length], buffer, 0);

    function fill(sliceId, numSlices, warmup) {
      var indexPos = checkpoints[sliceId];
      var indexEnd = targetsLength;
      if (warmup)
        indexEnd = std_Math_min(indexEnd, indexPos + CHUNK_SIZE);

      // Range in the output for which we are responsible:
      var [outputStart, outputEnd] = ComputeSliceBounds(length, sliceId, numSlices);

      for (; indexPos < indexEnd; indexPos++) {
        var x = self.get(indexPos);
        var t = checkTarget(indexPos, targets[indexPos]);
        if (t < outputStart || t >= outputEnd)
          continue;
        if (conflicts[t])
          x = collide(x, buffer[t]);
        UnsafeSetElement(buffer, t, x,
                         conflicts, t, true,
                         checkpoints, sliceId, indexPos + 1);
      }
    }
  }

  function parDivideScatterVector() {
    // Subtle: because we will be mutating the localBuffers and
    // conflict arrays in place, we can never replay an entry in the
    // target array for fear of inducing a conflict where none existed
    // before. Therefore, we must proceed not by chunks but rather by
    // individual indices.
    var numSlices = ParallelSlices();
    var info = ComputeAllSliceBounds(targetsLength, numSlices);

    // FIXME(bug 844890): Use typed arrays here.
    var localBuffers = NewDenseArray(numSlices);
    for (var i = 0; i < numSlices; i++)
      UnsafeSetElement(localBuffers, i, NewDenseArray(length));
    var localConflicts = NewDenseArray(numSlices);
    for (var i = 0; i < numSlices; i++) {
      var conflicts_i = NewDenseArray(length);
      for (var j = 0; j < length; j++)
        UnsafeSetElement(conflicts_i, j, false);
      UnsafeSetElement(localConflicts, i, conflicts_i);
    }

    // Initialize the 0th buffer, which will become the output. For
    // the other buffers, we track which parts have been written to
    // using the conflict buffer so they do not need to be
    // initialized.
    var outputBuffer = localBuffers[0];
    for (var i = 0; i < length; i++)
      UnsafeSetElement(outputBuffer, i, defaultValue);

    ParallelDo(fill, CheckParallel(mode));
    mergeBuffers();
    return NewParallelArray(ParallelArrayView, [length], outputBuffer, 0);

    function fill(sliceId, numSlices, warmup) {
      var indexPos = info[SLICE_POS(sliceId)];
      var indexEnd = info[SLICE_END(sliceId)];
      if (warmup)
        indexEnd = std_Math_min(indexEnd, indexPos + CHUNK_SIZE);

      var localbuffer = localBuffers[sliceId];
      var conflicts = localConflicts[sliceId];
      while (indexPos < indexEnd) {
        var x = self.get(indexPos);
        var t = checkTarget(indexPos, targets[indexPos]);
        if (conflicts[t])
          x = collide(x, localbuffer[t]);
        UnsafeSetElement(localbuffer, t, x,
                         conflicts, t, true,
                         info, SLICE_POS(sliceId), ++indexPos);
      }
    }

    /**
     * Merge buffers 1..NUMSLICES into buffer 0. In principle, we could
     * parallelize the merge work as well. But for this first cut,
     * just do the merge sequentially.
     */
    function mergeBuffers() {
      var buffer = localBuffers[0];
      var conflicts = localConflicts[0];
      for (var i = 1; i < numSlices; i++) {
        var otherbuffer = localBuffers[i];
        var otherconflicts = localConflicts[i];
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
    var buffer = NewDenseArray(length);
    var conflicts = NewDenseArray(length);

    for (var i = 0; i < length; i++) {
      UnsafeSetElement(buffer, i, defaultValue);
      UnsafeSetElement(conflicts, i, false);
    }

    for (var i = 0; i < targetsLength; i++) {
      var x = self.get(i);
      var t = checkTarget(i, targets[i]);
      if (conflicts[t])
        x = collide(x, buffer[t]);

      UnsafeSetElement(buffer, t, x,
                       conflicts, t, true);
    }

    return NewParallelArray(ParallelArrayView, [length], buffer, 0);
  }

  function checkTarget(i, t) {
    if (TO_INT32(t) !== t)
      ThrowError(JSMSG_PAR_ARRAY_SCATTER_BAD_TARGET, i);

    if (t < 0 || t >= length)
      ThrowError(JSMSG_PAR_ARRAY_SCATTER_BOUNDS);

    // It's not enough to return t, as -0 | 0 === -0.
    return TO_INT32(t);
  }
}

/**
 * The familiar filter() operation applied across the outermost
 * dimension.
 */
function ParallelArrayFilter(func, mode) {
  // FIXME(bug 844887): Check |this instanceof ParallelArray|
  // FIXME(bug 844887): Check |IsCallable(func)|

  var self = this;
  var length = self.shape[0];

  parallel: for (;;) { // see ParallelArrayBuild() to explain why for(;;) etc
    if (ShouldForceSequential())
      break parallel;
    if (!TRY_PARALLEL(mode))
      break parallel;

    var chunks = ComputeNumChunks(length);
    var numSlices = ParallelSlices();
    if (chunks < numSlices * 2)
      break parallel;

    var info = ComputeAllSliceBounds(chunks, numSlices);

    // Step 1. Compute which items from each slice of the result
    // buffer should be preserved. When we're done, we have an array
    // |survivors| containing a bitset for each chunk, indicating
    // which members of the chunk survived. We also keep an array
    // |counts| containing the total number of items that are being
    // preserved from within one slice.
    //
    // FIXME(bug 844890): Use typed arrays here.
    var counts = NewDenseArray(numSlices);
    for (var i = 0; i < numSlices; i++)
      UnsafeSetElement(counts, i, 0);
    var survivors = NewDenseArray(chunks);
    ParallelDo(findSurvivorsInSlice, CheckParallel(mode));

    // Step 2. Compress the slices into one contiguous set.
    var count = 0;
    for (var i = 0; i < numSlices; i++)
      count += counts[i];
    var buffer = NewDenseArray(count);
    if (count > 0)
      ParallelDo(copySurvivorsInSlice, CheckParallel(mode));

    return NewParallelArray(ParallelArrayView, [count], buffer, 0);
  }

  // Sequential fallback:
  ASSERT_SEQUENTIAL_IS_OK(mode);
  var buffer = [];
  for (var i = 0; i < length; i++) {
    var elem = self.get(i);
    if (func(elem, i, self))
      ARRAY_PUSH(buffer, elem);
  }
  return NewParallelArray(ParallelArrayView, [buffer.length], buffer, 0);

  /**
   * As described above, our goal is to determine which items we
   * will preserve from a given slice. We do this one chunk at a
   * time. When we finish a chunk, we record our current count and
   * the next chunk sliceId, lest we should bail.
   */
  function findSurvivorsInSlice(sliceId, numSlices, warmup) {

    var chunkPos = info[SLICE_POS(sliceId)];
    var chunkEnd = info[SLICE_END(sliceId)];

    if (warmup && chunkEnd > chunkPos)
      chunkEnd = chunkPos + 1;

    var count = counts[sliceId];
    while (chunkPos < chunkEnd) {
      var indexStart = chunkPos << CHUNK_SHIFT;
      var indexEnd = std_Math_min(indexStart + CHUNK_SIZE, length);
      var chunkBits = 0;

      for (var bit = 0; indexStart + bit < indexEnd; bit++) {
        var keep = !!func(self.get(indexStart + bit), indexStart + bit, self);
        chunkBits |= keep << bit;
        count += keep;
      }

      UnsafeSetElement(survivors, chunkPos, chunkBits,
                       counts, sliceId, count,
                       info, SLICE_POS(sliceId), ++chunkPos);
    }
  }

  function copySurvivorsInSlice(sliceId, numSlices, warmup) {
    // Copies the survivors from this slice into the correct position.
    // Note that this is an idempotent operation that does not invoke
    // user code. Therefore, we don't expect bailouts and make an
    // effort to proceed chunk by chunk or avoid duplicating work.

    // During warmup, we only execute with sliceId 0. This would fail to
    // execute the loop below. Therefore, during warmup, we
    // substitute 1 for the sliceId.
    if (warmup && sliceId == 0 && numSlices != 1)
      sliceId = 1;

    // Total up the items preserved by previous slices.
    var count = 0;
    if (sliceId > 0) { // FIXME(#819219)---work around a bug in Ion's range checks
      for (var i = 0; i < sliceId; i++)
        count += counts[i];
    }

    // Compute the final index we expect to write.
    var total = count + counts[sliceId];
    if (count == total)
      return;

    // Iterate over the chunks assigned to us. Read the bitset for
    // each chunk. Copy values where a 1 appears until we have
    // written all the values that we expect to. We can just iterate
    // from 0...CHUNK_SIZE without fear of a truncated final chunk
    // because we are already checking for when count==total.
    var chunkStart = info[SLICE_START(sliceId)];
    var chunkEnd = info[SLICE_END(sliceId)];
    for (var chunk = chunkStart; chunk < chunkEnd; chunk++) {
      var chunkBits = survivors[chunk];
      if (!chunkBits)
        continue;

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

/**
 * Divides the outermost dimension into two dimensions. Does not copy
 * or affect the underlying data, just how it is divided amongst
 * dimensions. So if we had a vector with shape [N, ...] and you
 * partition with amount=4, you get a [N/4, 4, ...] vector. Note that
 * N must be evenly divisible by 4 in that case.
 */
function ParallelArrayPartition(amount) {
  if (amount >>> 0 !== amount)
    ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");

  var length = this.shape[0];
  var partitions = (length / amount) | 0;

  if (partitions * amount !== length)
    ThrowError(JSMSG_PAR_ARRAY_BAD_PARTITION);

  var shape = [partitions, amount];
  for (var i = 1; i < this.shape.length; i++)
    ARRAY_PUSH(shape, this.shape[i]);
  return NewParallelArray(ParallelArrayView, shape, this.buffer, this.offset);
}

/**
 * Collapses two outermost dimensions into one. So if you had
 * a [X, Y, ...] vector, you get a [X*Y, ...] vector.
 */
function ParallelArrayFlatten() {
  if (this.shape.length < 2)
    ThrowError(JSMSG_PAR_ARRAY_ALREADY_FLAT);

  var shape = [this.shape[0] * this.shape[1]];
  for (var i = 2; i < this.shape.length; i++)
    ARRAY_PUSH(shape, this.shape[i]);
  return NewParallelArray(ParallelArrayView, shape, this.buffer, this.offset);
}

//
// Accessors and utilities.
//

/**
 * Specialized variant of get() for one-dimensional case
 */
function ParallelArrayGet1(i) {
  if (i === undefined)
    return undefined;
  return this.buffer[this.offset + i];
}

function MatrixGet1(i) {
  if (i === undefined)
    return undefined;
  return this.buffer[this.offset + i];
}

/**
 * Specialized variant of get() for two-dimensional case
 */
function ParallelArrayGet2(x, y) {
  var xDimension = this.shape[0];
  var yDimension = this.shape[1];
  if (x === undefined)
    return undefined;
  if (x >= xDimension)
    return undefined;
  if (y === undefined)
    return NewParallelArray(ParallelArrayView, [yDimension], this.buffer, this.offset + x * yDimension);
  if (y >= yDimension)
    return undefined;
  var offset = y + x * yDimension;
  return this.buffer[this.offset + offset];
}

function MatrixGet2(x, y) {
  var xDimension = this.shape[0];
  var yDimension = this.shape[1];
  if (x === undefined)
    return undefined;
  if (x >= xDimension)
    return undefined;
  if (y === undefined)
    return NewMatrix(MatrixView, [yDimension], this.buffer, this.offset + x * yDimension,
                             this.valtype);
  if (y >= yDimension)
    return undefined;
  var offset = y + x * yDimension;
  return this.buffer[this.offset + offset];
}

/**
 * Specialized variant of get() for three-dimensional case
 */
function ParallelArrayGet3(x, y, z) {
  var xDimension = this.shape[0];
  var yDimension = this.shape[1];
  var zDimension = this.shape[2];
  if (x === undefined)
    return undefined;
  if (x >= xDimension)
    return undefined;
  if (y === undefined)
    return NewParallelArray(ParallelArrayView, [yDimension, zDimension],
                            this.buffer, this.offset + x * yDimension * zDimension);
  if (y >= yDimension)
    return undefined;
  if (z === undefined)
    return NewParallelArray(ParallelArrayView, [zDimension],
                            this.buffer, this.offset + y * zDimension + x * yDimension * zDimension);
  if (z >= zDimension)
    return undefined;
  var offset = z + y*zDimension + x * yDimension * zDimension;
  return this.buffer[this.offset + offset];
}

function MatrixGet3(x, y, z) {
  var xDimension = this.shape[0];
  var yDimension = this.shape[1];
  var zDimension = this.shape[2];
  if (x === undefined)
    return undefined;
  if (x >= xDimension)
    return undefined;
  if (y === undefined)
    return NewMatrix(MatrixView, [yDimension, zDimension],
                            this.buffer, this.offset + x * yDimension * zDimension,
                            this.valtype);
  if (y >= yDimension)
    return undefined;
  if (z === undefined)
    return NewMatrix(MatrixView, [zDimension],
                            this.buffer, this.offset + y * zDimension + x * yDimension * zDimension,
                            this.valtype);
  if (z >= zDimension)
    return undefined;
  var offset = z + y*zDimension + x * yDimension * zDimension;
  return this.buffer[this.offset + offset];
}

/**
 * Generalized version of get() for N-dimensional case
 */
function ParallelArrayGetN(...coords) {
  if (coords.length == 0)
    return undefined;

  var products = ComputeProducts(this.shape);

  // Compute the offset of the given coordinates. Each index is
  // multipled by its corresponding entry in the |products|
  // array, counting in reverse. So if |coords| is [a,b,c,d],
  // then you get |a*BCD + b*CD + c*D + d|.
  var offset = this.offset;
  var sDimensionality = this.shape.length;
  var cDimensionality = coords.length;
  for (var i = 0; i < cDimensionality; i++) {
    if (coords[i] >= this.shape[i])
      return undefined;
    offset += coords[i] * products[sDimensionality - i - 1];
  }

  if (cDimensionality < sDimensionality) {
    var shape = callFunction(std_Array_slice, this.shape, cDimensionality);
    return NewParallelArray(ParallelArrayView, shape, this.buffer, offset);
  }
  return this.buffer[offset];
}

function MatrixGetN(...coords) {
  if (coords.length == 0)
    return undefined;

  var products = ComputeProducts(this.shape);

  // Compute the offset of the given coordinates. Each index is
  // multipled by its corresponding entry in the |products|
  // array, counting in reverse. So if |coords| is [a,b,c,d],
  // then you get |a*BCD + b*CD + c*D + d|.
  var offset = this.offset;
  var sDimensionality = this.shape.length;
  var cDimensionality = coords.length;
  for (var i = 0; i < cDimensionality; i++) {
    if (coords[i] >= this.shape[i])
      return undefined;
    offset += coords[i] * products[sDimensionality - i - 1];
  }

  if (cDimensionality < sDimensionality) {
    var shape = callFunction(std_Array_slice, this.shape, cDimensionality);
    return NewMatrix(MatrixView, shape, this.buffer, offset,
                             this.valtype);
  }
  return this.buffer[offset];
}

/** The length property yields the outermost dimension */
function ParallelArrayLength() {
  return this.shape[0];
}

function ParallelArrayToString() {
  var l = this.length;
  if (l == 0)
    return "";

  var open, close;
  if (this.shape.length > 1) {
    open = "<";
    close = ">";
  } else {
    open = close = "";
  }

  var result = "";
  for (var i = 0; i < l - 1; i++) {
    result += open + String(this.get(i)) + close;
    result += ",";
  }
  result += open + String(this.get(l - 1)) + close;
  return result;
}

// Fills buffer starting from offset under the assumption that
// it has shape = frame x grain at that point, using func to
// generate each element (of size grain) in the iteration space
// defined by frame.
function MatrixPFill(parexec, buffer, offset, shape, frame, grain, valtype, func, mode)
{
  mode && mode.print && mode.print({called:"PMF A1", buffer:buffer,
                                    offset:offset, frame:frame, grain:grain});

  var i;
  var frame_len = ProductOfArrayRange(frame, 0, frame.length);
  for(i = frame_dims; i < shape.length; i++) {
    var shape_amt = shape[i];
    if (i == shape.length - 1 && typeof(shape_amt) !== "number")
      shape_amt = 1;
  }

  var indexStart = offset;
  var indexEnd = offset+frame_len;
  var grain_len = ProductOfArrayRange(grain, 0, grain.length);

  mode && mode.print && mode.print(
    {called:"PMF B", buffer:buffer, offset:offset, frame:frame, grain:grain,
     frame_len:frame_len, grain_len:grain_len, indexStart:indexStart, indexEnd:indexEnd});

  var computefunc;
  var isLeaf = (grain.length == 0);
  switch (frame.length) {
/*
   case 1:
    computefunc = isLeaf ? fill1_leaf : fill1_subm;
    mode && mode.print && mode.print({called:"MatrixPFill computefunc is fill1"});
    break;
*/
/*
  case 2:
    computefunc = isLeaf ? fill2_leaf : fill2_subm;
    break;
  case 3:
    computefunc = isLeaf ? fill3_leaf : fill3_subm;
    break;
*/
  default:
    computefunc = isLeaf ? fillN_leaf : fillN_subm;
    mode && mode.print && mode.print({called:"MatrixPFill computefunc is fillN"});
    break;
  }

  mode && mode.print && mode.print({called:"MatrixPFill prior parallel"});

  parallel: for(;;) { // see ParallelArrayBuild() to explain why for(;;) etc
    if (!parexec)
      break parallel;
    if (ShouldForceSequential())
      break parallel;
    if (!TRY_PARALLEL(mode))
      break parallel;
    if (computefunc === fillN_leaf || computefunc === fillN_subm)
      break parallel;

    var chunks = ComputeNumChunks(frame_len);
    var numSlices = ParallelSlices();
    var info = ComputeAllSliceBounds(chunks, numSlices);
    ParallelDo(constructSlice, CheckParallel(mode));
    return;
  }

  mode && mode.print && mode.print({called:"MatrixPFill seq fallback", frame_len:frame_len, indexStart:indexStart, indexEnd:indexEnd});

  // Sequential fallback:
  ASSERT_SEQUENTIAL_IS_OK(mode);
  computefunc(indexStart, indexEnd);
  return;

  function constructSlice(sliceId, numSlices, warmup) {

    var chunkPos = info[SLICE_POS(sliceId)];
    var chunkEnd = info[SLICE_END(sliceId)];

    if (warmup && chunkEnd > chunkPos)
      chunkEnd = chunkPos + 1;

    while (chunkPos < chunkEnd) {
      var indexStart = chunkPos << CHUNK_SHIFT;
      var indexEnd = std_Math_min(indexStart + CHUNK_SIZE, frame_len);
      computefunc(indexStart, indexEnd);
      UnsafeSetElement(info, SLICE_POS(sliceId), ++chunkPos);
    }
  }

  function fill1_leaf(indexStart, indexEnd) {
    mode && mode.print && mode.print({called: "fill1_leaf A", buffer:buffer, indexStart: indexStart, indexEnd: indexEnd});

    for (var i = indexStart; i < indexEnd; i++) {
      UnsafeSetElement(buffer, i, func(i));
    }
  }

  function fill1_subm(indexStart, indexEnd) {

    mode && mode.print && mode.print({called: "fill1_subm A", indexStart: indexStart, indexEnd: indexEnd});

    var bufoffset = indexStart;
    for (var i = indexStart; i < indexEnd; i++, bufoffset += grain_len) {
      mode && mode.print && mode.print({called: "fill1_subm B", i:i,
                                        bufoffset:bufoffset,
                                        indexStart: indexStart,
                                        indexEnd: indexEnd});

      var subarray = func(i);
      var [subbuffer, suboffset] =
        IdentifySubbufferAndSuboffset(subarray);

      CopyFromSubbuffer(buffer, bufoffset, subbuffer, suboffset);
    }
  }

  function fill2_leaf(indexStart, indexEnd) {
    var x = (indexStart / yDimension) | 0;
    var y = indexStart - x*yDimension;
    for (var i = indexStart; i < indexEnd; i++) {
      UnsafeSetElement(buffer, i, func(x, y));
      if (++y == yDimension) {
        y = 0;
        ++x;
      }
    }
  }

  function fill2_subm(indexStart, indexEnd) {
    var bufoffset = indexStart;
    var x = (indexStart / yDimension) | 0;
    var y = indexStart - x*yDimension;
    for (var i = indexStart; i < indexEnd; i++, bufoffset += grain_len) {
      var subarray = func(x, y);
      var [subbuffer, suboffset] =
        IdentifySubbufferAndSuboffset(subarray);
      CopyFromSubbuffer(buffer, bufoffset, subbuffer, suboffset);
      if (++y == yDimension) {
        y = 0;
        ++x;
      }
    }
  }

  function fill3_leaf(indexStart, indexEnd) {
    var x = (indexStart / (yDimension*zDimension)) | 0;
    var r = indexStart - x*yDimension*zDimension;
    var y = (r / zDimension) | 0;
    var z = r - y*zDimension;
    for (var i = indexStart; i < indexEnd; i++) {
      UnsafeSetElement(buffer, i, func(x, y, z));
      if (++z == zDimension) {
        z = 0;
        if (++y == yDimension) {
          y = 0;
          ++x;
        }
      }
    }
  }

  function fill3_subm(indexStart, indexEnd) {
    var bufoffset = indexStart;
    var x = (indexStart / (yDimension*zDimension)) | 0;
    var r = indexStart - x*yDimension*zDimension;
    var y = (r / zDimension) | 0;
    var z = r - y*zDimension;
    for (var i = indexStart; i < indexEnd; i++, bufoffset += grain_len) {
      var subarray = func(x, y, z);
      var [subbuffer, suboffset] =
        IdentifySubbufferAndSuboffset(subarray);
      CopyFromSubbuffer(buffer, bufoffset, subbuffer, suboffset);
      if (++z == zDimension) {
        z = 0;
        if (++y == yDimension) {
          y = 0;
          ++x;
        }
      }
    }
  }

  function fillN_leaf(indexStart, indexEnd) {
    mode && mode.print && mode.print({called: "fillN_leaf A", offset:offset, indexStart: indexStart, indexEnd: indexEnd, frame:frame});
    var frame_indices = ComputeIndices(frame, indexStart);
    mode && mode.print && mode.print({called: "fillN_leaf B", frame_indices:frame_indices});
    var used_outptr = false;
    function set(v) { UnsafeSetElement(buffer, i, val); used_outptr = true; }
    for (i = indexStart; i < indexEnd; i++) {
      used_outptr = false;
      var outptr = {};
      outptr.set = set;
      frame_indices.push(outptr);
      mode && mode.print && mode.print({called: "fillN_leaf C", i:i, frame_indices:frame_indices});
      var val = func.apply(undefined, frame_indices);
      mode && mode.print && mode.print({called: "fillN_leaf C", i:i, frame_indices:frame_indices, val:val});
      if (!used_outptr) {
        UnsafeSetElement(buffer, i, val);
      }
      frame_indices.pop();
      StepIndices(frame, frame_indices);
    }
  }

  function fillN_subm(indexStart, indexEnd) {

    mode && mode.print && mode.print({called: "fillN_subm A", offset:offset, indexStart: indexStart, indexEnd: indexEnd});

    var bufoffset = indexStart;

    // allocate new arrays and copy in computed subarrays.
    var frame_indices = ComputeIndices(frame, indexStart);
    var used_outptr = false;

    function ndimIndexToOffset(...indices) {
      mode && mode.print && mode.print({called: "outptr.ndimIndexToOffset", indices:indices});
      var accum_idx  = 0;
      var accum_prod = 1;
      for (var i=indices.length-1; i>=0; i--) {
        var arg_i = indices[i];
        var grain_i = grain[i];
        if (arg_i >= grain_i) {
          ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, ": outptr.set index too large");
        }
        accum_idx += arg_i * accum_prod;
        accum_prod *= grain_i;
      }
      return accum_idx;
    }

    function outptr_set(...args) {
      mode && mode.print && mode.print({called: "outptr.set A", args:args});
      var v = args.pop();
      if (args.length > grain.length) {
        ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, ": too many args to outptr.set");
      }
      if (args.length < grain.length) {
        ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, ": outptr.set curry not yet unsupported");
      }
      var offset = ndimIndexToOffset.apply(null, args);
      mode && mode.print && mode.print({called: "outptr.set Y", bufoffset: bufoffset, offset: offset, sum: bufoffset + offset, v:v});
      UnsafeSetElement(buffer, bufoffset + offset, v);
      used_outptr = true;
    }

    function outptr_gather(arg0, arg1, arg2) { // ([depth,] func, [mode])
      mode && mode.print && mode.print({called: "outptr.gather A", arg0:arg0, arg1:arg1, arg2:arg2});
      var depth, func, mode;
      if (typeof arg0 === "function") {
        depth = grain.length;
        func = arg0;
        mode = arg1;
      } else { // assumes (typeof arg1 === "function")
        depth = arg0;
        func = arg1;
        mode = arg2;
      }

      var subframe = grain.slice(0, depth);
      var subgrain = grain.slice(depth);

      mode && mode.print && mode.print({called: "outptr.gather", bufoffset:bufoffset, depth:depth, subframe:subframe, subgrain:subgrain});

      MatrixPFill(true, buffer, bufoffset, grain, subframe, subgrain, valtype, func, mode);
      used_outptr = true;
    }

    // FIXME: Something seems off about handling of i, indexStart, bufoffset...
    for (i = indexStart; i < indexEnd; i++, bufoffset += grain_len) {
      used_outptr = false;
      var outptr = {};
      outptr.set = outptr_set;
      outptr.gather = outptr_gather;
      frame_indices.push(outptr);

      mode && mode.print && mode.print({called: "fillN_subm C", i:i, frame_indices:frame_indices});

      var subarray = func.apply(null, frame_indices);
      frame_indices.pop();

      mode && mode.print && mode.print({called: "fillN_subm D", subarray:subarray});

      if (!used_outptr) {
        var [subbuffer, suboffset] = IdentifySubbufferAndSuboffset(subarray);
        mode && mode.print && mode.print({called: "fillN_subm E", subbuffer:subbuffer, suboffset:suboffset});
        CopyFromSubbuffer(buffer, bufoffset, subbuffer, suboffset);
      }

      mode && mode.print && mode.print({called: "fillN_subm F"});

      StepIndices(frame, frame_indices);
    }
  }

  function IdentifySubbufferAndSuboffset(subarray) {
    var suboffset;
    var subbuffer;

    if (std_Array_isArray(subarray)) {
      subbuffer = subarray;
      suboffset = 0;
    } else if (IsParallelArray(subarray) || IsMatrix(subarray)) {
      var subvaltype;
      if (IsParallelArray(subarray)) {
         subvaltype = "any";
      } else {
         subvaltype = subarray.valtype;
      }
      if (!submatrix_matches_expectation(grain, valtype, subarray.shape, subvaltype)) {
        ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, " mismatched submatrix returned"+
                                            " with shape: ["+subarray.shape+","+subvaltype+"];"+
                                            " expected submatrix with shape: ["+grain+","+valtype+"]");
      }
      subbuffer = subarray.buffer;
      suboffset = subarray.offset;
    } else {
      ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, " non-submatrix returned: "+subarray+" expected submatrix with shape: ["+grain+"]");
    }

    return [subbuffer, suboffset];
  }

  function CopyFromSubbuffer(buffer, bufoffset, subbuffer, suboffset) {
    UnsafeArrayCopy(buffer, bufoffset, subbuffer, suboffset, grain_len);
  }
}

function is_value_type(descriptor) {
  if (typeof descriptor === "string") {
    if (descriptor === "uint8" ||
        descriptor === "uint8clamped" ||
        descriptor === "uint16" ||
        descriptor === "uint32" ||
        descriptor === "int8" ||
        descriptor === "int16" ||
        descriptor === "int32" ||
        descriptor === "float32" ||
        descriptor === "float64" ||
        descriptor === "any")
    {
      return true;
    }
    else
    {
      ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, ' invalid data type specification "'+descriptor+'"');
    }
  }
  else
  {
    // Other cases (e.g. for Data Type objects) could go here
  }

  return false;
}

function submatrix_matches_expectation(expect_shape, expect_valtype, actual_shape, actual_valtype)
{
  if (expect_shape.length !== actual_shape.length)
    return false;
  if (expect_valtype !== actual_valtype)
    return false;
  var len = expect_shape.length;
  for (var i = 0; i < len; i++) {
    if (expect_shape[i] !== actual_shape[i]) {
      return false;
    }
  }
  return true;
}

function value_type_to_buffer_allocator(descriptor) {
  function make_univ_buffer(length)         { return NewDenseArray(length); }
  function make_uint8_buffer(length)        { return new Uint8Array(new ArrayBuffer(length)); }
  function make_uint8clamped_buffer(length) { return new Uint8ClampedArray(new ArrayBuffer(length)); }
  function make_uint16_buffer(length)       { return new Uint16Array(new ArrayBuffer(length*2)); }
  function make_uint32_buffer(length)       { return new Uint32Array(new ArrayBuffer(length*4)); }
  function make_int8_buffer(length)         { return new Int8Array(new ArrayBuffer(length)); }
  function make_int16_buffer(length)        { return new Int16Array(new ArrayBuffer(length*2)); }
  function make_int32_buffer(length)        { return new Int32Array(new ArrayBuffer(length*4)); }
  function make_flo32_buffer(length)        { return new Float32Array(new ArrayBuffer(length*4)); }
  function make_flo64_buffer(length)        { return new Float64Array(new ArrayBuffer(length*8)); }

  var lookup = {
    uint8:   make_uint8_buffer,  uint8clamped: make_uint8clamped_buffer,
    uint16:  make_uint16_buffer, uint32:       make_uint32_buffer,
    int8:    make_int8_buffer,   int16:        make_int16_buffer,        int32: make_int32_buffer,
    float32: make_flo32_buffer,  float64:      make_flo64_buffer,        any: make_univ_buffer
  };

  return lookup[descriptor];
}

function make_buffer_from_shape_and_valtype(shape, descriptor) {
  var buffer_maker = value_type_to_buffer_allocator(descriptor);
  var elem_count = ProductOfArrayRange(shape, 0, shape.length);
  return buffer_maker(elem_count);
}

function MatrixConstructFromGrainFunctionMode(arg0, arg1, arg2, arg3) {
  // (shape, grain, func, mode)

  // The five properties of matrix under construction.
  var buffer;
  var offset = 0;
  var shape;
  var valtype;
  var getFunc;

  // Transient state used during construction.
  var frame = arg0 || [0];
  var grain;
  var func;
  var mode;

  if (typeof arg1 === "function") {
    grain = [];
    func = arg1;
    mode = arg2;
  } else {
    grain = arg1;
    func = arg2;
    mode = arg3;
  }

  if (func === undefined) {
    func = function fill_with_undef () { return undefined; };
  }

  mode && mode.print && mode.print({called:"PMC A", frame:frame, grain:grain});

  check_frame_argument_is_valid(frame);

  valtype = pop_valtype(grain);
  shape = frame.concat(grain);

  mode && mode.print && mode.print({called:"PMC B", shape:shape, valtype:valtype});

  buffer = make_buffer_from_shape_and_valtype(shape, valtype);

  switch(shape.length) {
    case 1: getFunc = MatrixGet1; break;
    case 2: getFunc = MatrixGet2; break;
    case 3: getFunc = MatrixGet3; break;
    default: getFunc = MatrixGetN; break;
  }

  mode && mode.print && mode.print({called:"PMC C", shape:shape, valtype:valtype, buffer:buffer});

  // Eventually specialize on particular shape.length's (1, 2, ...).
  // But more important for now to get semantics of general
  // case right (and also parallelize on at least *one* case).

  MatrixPFill(true, buffer, offset, shape, frame, grain, valtype, func, mode);
  setup_fields_in_this(this);
  return this;

  function pop_valtype(grain) {
    if (is_value_type(grain[grain.length - 1])) {
      return grain.pop();
    } else {
      return "any"; // might express as "ObjectPointer" from binary data spec
    }
  }

  function check_frame_argument_is_valid(frame) {
    if (!std_Array_isArray(frame)) {
      ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, ": frame argument "+frame+" is not an array of dimensions");
    }

    if (is_value_type(frame[frame.length - 1])) {
      var desc = frame[frame.length - 1];
      ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, ' data type specification ("'+desc+'") should only occur in grain argument');
    }
  }

  function setup_fields_in_this(self) {
    self.buffer = buffer;
    self.offset = offset;
    self.shape = shape;
    self.valtype = valtype;
    self.get = getFunc;
  }
}

// Analogous to ParallelArrayView
function MatrixView(shape, buffer, offset, valtype)
{
  this.shape = shape;
  this.buffer = buffer;
  this.offset = offset;
  this.valtype = valtype;

  switch(shape.length) {
    case 1: this.get = MatrixGet1; break;
    case 2: this.get = MatrixGet2; break;
    case 3: this.get = MatrixGet3; break;
    default: this.get = MatrixGetN; break;
  }

  return this;
}

// Idea: If shape is length L, then depth is either a positive integer <= L
// or a negative integer >= -L+1.  Depth defaults to L.  A negative depth
// is interpreted as L+depth.  This way one can generically map over e.g.
// ARGBV arrays at the leaves of the iteration space.
// grain is the expected type of the *result* from invoking func.
function MatrixCommonMap(self, parexec, depth, grain, func, mode) {

  mode && mode.print && mode.print({called:"PMM A", depth:depth, grain:grain});

  var frame = self.shape.slice(0, depth);
  var indices = ComputeIndices(frame, 0);

  var valtype;
  if (is_value_type(grain[grain.length - 1])) {
    valtype = grain.pop();
    mode && mode.print && mode.print({called:"PMM B", expl_valtype:valtype});
  } else {
    valtype = "any"; // this might be expressible as "ObjectPointer" in binary data spec
    mode && mode.print && mode.print({called:"PMM B", impl_valtype:valtype});
  }

  var buffer_maker = value_type_to_buffer_allocator(valtype);
  var shape = frame.concat(grain);
  var len = ProductOfArrayRange(shape, 0, shape.length);
  var buffer = buffer_maker(len);
  var offset = 0;

  function fill1(i) {
    mode && mode.print && mode.print({called:"fill1",i:i});
    return func(self.get(i), i); }
  function fill2(i, j) {
    mode && mode.print && mode.print({called:"fill2",i:i,j:j});
    return func(self.get(i, j), i, j); }
  function fill3(i, j, k) {
    mode && mode.print && mode.print({called:"fill3",i:i,j:j,k:k});
    return func(self.get(i, j, k), i, j, k); }
  function fillN(...args) {
    mode && mode.print && mode.print({called:"fillN",args:args});
    return func.apply(undefined, self.get.apply(self, args), args); }
  var fill;
  switch (frame.length) {
    case 1:  fill = fill1; break;
    case 2:  fill = fill2; break;
    case 3:  fill = fill3; break;
    default: fill = fillN; break;
  }

  MatrixPFill(parexec, buffer, offset, shape, frame, grain, valtype, fill, mode);
  return NewMatrix(MatrixView, shape, buffer, offset, valtype);

}

function MatrixDecomposeArgsForMap(self, arg0, arg1, arg2, arg3) { // ([depth,] [output-grain-type,] func, [mode])
  var depth = self.shape.length;
  var grain = ["any"];
  var func, mode;
  if (typeof arg0 === "function") {
    func = arg0;
    mode = arg1;
  } else if (typeof arg1 === "function") {
    if (std_Array_isArray(arg0)) {
      grain = arg0;
    } else {
      depth = arg0;
    }
    func = arg1;
    mode = arg2;
  } else { // assumes (typeof arg2 === "function")
    depth = arg0;
    grain = arg1;
    func = arg2;
    mode = arg3;
  }

  return [depth, grain, func, mode];
}

function MatrixPMap(arg0, arg1, arg2, arg3) { // ([depth,] [output-grain-type,] func, [mode])
  var [depth, outputgrain, func, mode] =
    MatrixDecomposeArgsForMap(this, arg0, arg1, arg2, arg3);
  return MatrixCommonMap(this, true, depth, grain, func, mode);
}

function MatrixMap(arg0, arg1, arg2, arg3) { // ([depth,] [output-grain-type,] func, [mode])
  var [depth, grain, func, mode] =
    MatrixDecomposeArgsForMap(this, arg0, arg1, arg2, arg3);
  return MatrixCommonMap(this, false, depth, grain, func, mode);
}

function MatrixCommonReduceScalar(self, parexec, func, mode) {

  var depth = self.shape.length;
  mode && mode.print && mode.print({where:"MatrixReduce", depth:depth, func:func, mode:mode, self:self});
  var shape = self.shape;
  var length = ProductOfArrayRange(shape, 0, shape.length);
  if (length === 0)
    ThrowError(JSMSG_PAR_ARRAY_REDUCE_EMPTY);

  parallel: for (;;) { // see ParallelArrayBuild() to explain why for(;;) etc
    if (!parexec)
      break parallel;
    if (ShouldForceSequential())
      break parallel;
    if (!TRY_PARALLEL(mode))
      break parallel;

    var chunks = ComputeNumChunks(length);
    var numSlices = ParallelSlices();
    if (chunks < numSlices)
      break parallel;

    var info = ComputeAllSliceBounds(chunks, numSlices);
    var subreductions = NewDenseArray(numSlices);
    ParallelDo(reduceSlice, CheckParallel(mode));
    var accumulator = subreductions[0];
    for (var i = 1; i < numSlices; i++)
      accumulator = func(accumulator, subreductions[i]);
    return accumulator;
  }

  // Sequential fallback:
  ASSERT_SEQUENTIAL_IS_OK(mode);
  var accumulator = self.buffer[self.offset];
  for (var i = 1; i < length; i++)
    accumulator = func(accumulator, self.buffer[self.offset+i]);
  return accumulator;

  function reduceSlice(sliceId, numSlices, warmup) {
    var chunkStart = info[SLICE_START(sliceId)];
    var chunkPos = info[SLICE_POS(sliceId)];
    var chunkEnd = info[SLICE_END(sliceId)];

    // (*) This function is carefully designed so that the warmup
    // (which executes with chunkStart === chunkPos) will execute all
    // potential loads and stores. In particular, the warmup run
    // processes two chunks rather than one. Moreover, it stores
    // accumulator into subreductions and then loads it again to
    // ensure that the load is executed during the warmup, as it will
    // certainly be executed during subsequent runs.

    if (warmup && chunkEnd > chunkPos + 2)
      chunkEnd = chunkPos + 2;

    if (chunkStart === chunkPos) {
      var indexPos = chunkStart << CHUNK_SHIFT;
      var accumulator = reduceChunk(self.buffer[self.offset+indexPos], indexPos + 1, indexPos + CHUNK_SIZE);

      UnsafeSetElement(subreductions, sliceId, accumulator, // see (*) above
                       info, SLICE_POS(sliceId), ++chunkPos);
    }

    var accumulator = subreductions[sliceId]; // see (*) above

    while (chunkPos < chunkEnd) {
      var indexPos = chunkPos << CHUNK_SHIFT;
      accumulator = reduceChunk(accumulator, indexPos, indexPos + CHUNK_SIZE);
      UnsafeSetElement(subreductions, sliceId, accumulator,
                       info, SLICE_POS(sliceId), ++chunkPos);
    }
  }

  function reduceChunk(accumulator, from, to) {
    to = std_Math_min(to, length);
    for (var i = from; i < to; i++)
      accumulator = func(accumulator, self.buffer[self.offset+i]);
    return accumulator;
  }
}

function MatrixCommonReduce(self, parexec, depth, func, mode) {
  mode && mode.print && mode.print({where:"MatrixCommonReduce", depth:depth, func:func, mode:mode, self:self});

  if (depth === self.shape.length)
    return MatrixCommonReduceScalar(self, parexec, func, mode);

  var shape = self.shape;
  var frame = shape.slice(0, depth);
  var grain = shape.slice(depth);
  var valtype = self.valtype;
  var length = ProductOfArrayRange(shape, 0, shape.length);
  if (length === 0)
    ThrowError(JSMSG_PAR_ARRAY_REDUCE_EMPTY);

  var used_outptr = false;

  function ndimIndexToOffset(...indices) {
    mode && mode.print && mode.print({called: "outptr.ndimIndexToOffset", indices:indices});
    var accum_idx  = 0;
    var accum_prod = 1;
    for (var i=indices.length-1; i>=0; i--) {
      var arg_i = indices[i];
      var grain_i = grain[i];
      if (arg_i >= grain_i) {
        ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, ": outptr.set index too large");
      }
      accum_idx += arg_i * accum_prod;
      accum_prod *= grain_i;
    }
    return accum_idx;
  }

  function outptr_set(...args) {
    mode && mode.print && mode.print({called: "outptr.set A", args:args});
    var v = args.pop();
    if (args.length > grain.length) {
      ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, ": too many args to outptr.set");
    }
    if (args.length < grain.length) {
      ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, ": outptr.set curry not yet unsupported");
    }
    var offset = ndimIndexToOffset.apply(null, args);
    mode && mode.print && mode.print({called: "outptr.set Y", bufoffset: bufoffset, offset: offset, sum: bufoffset + offset, v:v});
    UnsafeSetElement(buffer, bufoffset + offset, v);
    used_outptr = true;
  }

  function outptr_gather(arg0, arg1, arg2) { // ([depth,] func, [mode])
    mode && mode.print && mode.print({called: "outptr.gather A", arg0:arg0, arg1:arg1, arg2:arg2});
    var depth, func, mode;
    if (typeof arg0 === "function") {
      depth = grain.length;
      func = arg0;
      mode = arg1;
    } else { // assumes (typeof arg1 === "function")
      depth = arg0;
      func = arg1;
      mode = arg2;
    }

    var subframe = grain.slice(0, depth);
    var subgrain = grain.slice(depth);

    mode && mode.print && mode.print({called: "outptr.gather", bufoffset:bufoffset, depth:depth, subframe:subframe, subgrain:subgrain});

    MatrixPFill(true, buffer, bufoffset, grain, subframe, subgrain, valtype, func, mode);
    used_outptr = true;
  }

  mode && mode.print && mode.print({where:"MCR K", depth:depth, func:func, mode:mode, self:self});

  parallel: { /* no code yet */
    // Idea: preallocate N*3 result areas, where N is numSlices,
    // and each area can hold a matrix matching [grain, valtype]
    // Round-robin the use of the areas for intermediate results.
    //
    // Sequential code illustrates (simplified) round-robin.
    var numSlices = ParallelSlices();
  }

  mode && mode.print && mode.print({where:"MCR L", depth:depth, func:func, mode:mode, self:self});

  // Sequential fallback:
  var buf0 = make_buffer_from_shape_and_valtype(grain, valtype);
  var buf1 = make_buffer_from_shape_and_valtype(grain, valtype);
  var mat0 = NewMatrix(MatrixView, grain, buf0, 0, valtype);
  var mat1 = NewMatrix(MatrixView, grain, buf1, 0, valtype);
  var mat2 = NewMatrix(MatrixView, grain, self.buffer, self.offset, valtype);
  var grain_len = ProductOfArrayRange(grain, 0, grain.length);
  UnsafeArrayCopy(buf1, 0, self.buffer, self.offset, grain_len);
  var accumulator = self.buffer[self.offset];
  var out0 = {set:outptr_set, gather:outptr_gather, buffer:buf0};
  var out1 = {set:outptr_set, gather:outptr_gather, buffer:buf1};
  var outptrs = [out0, out1];
  var temps = [mat0, mat1];
  for (var i = 1; i < length; i++) {
    mat2.offset += grain_len;
    used_outptr = false;

    /* FIXME: must clear outptr[(i+1}%2] before letting control flow to func.*/
    var result = func(temps[i%2], mat2, outptrs[(i+1)%2]);
    if (!used_outptr) {
      var subarray = result;
      if (IsParallelArray(subarray) || IsMatrix(subarray)) {
        var subvaltype;
        if (IsParallelArray(subarray)) {
          subvaltype = "any";
        } else {
          subvaltype = subarray.valtype;
        }
        if (!submatrix_matches_expectation(grain, valtype, subarray.shape, subvaltype)) {
          ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, " mismatched submatrix returned"+
                                              " with shape: ["+subarray.shape+","+subvaltype+"];"+
                                              " expected submatrix with shape: ["+grain+","+valtype+"]");
        }
        UnsafeArrayCopy(outptrs[i%2].buffer, 0,
                        subarray.buffer, subarray.offset, grain_len);
      } else {
        ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, " non-submatrix returned: "+subarray+" expected submatrix with shape: ["+grain+"]");
      }
    }
  }

  mode && mode.print && mode.print({where:"MCR Z", depth:depth, func:func, mode:mode, self:self});

  return temps[(i+1)%2];
}

function MatrixDecomposeArgsForReduceOrScan(self, arg0, arg1, arg2) { // ([depth,] func, [mode])
  var depth = arg0;
  var func = arg1;
  var mode = arg2;

  if (typeof arg0 === "function") {
    // caller omitted depth argument; shift other arguments down
    depth = self.shape.length;
    func = arg0;
    mode = arg1;
  } else {
    depth = arg0;
    func = arg1;
    mode = arg2;
  }

  return [depth, func, mode];
}

function MatrixPReduce(arg0, arg1, arg2) { // ([depth,] func, [mode])
  var [depth, func, mode] =
    MatrixDecomposeArgsForReduceOrScan(this, arg0, arg1, arg2);
  return MatrixCommonReduce(this, true, depth, func, mode);
}

function MatrixReduce(arg0, arg1, arg2) { // ([depth,] func, [mode])
  var [depth, func, mode] =
    MatrixDecomposeArgsForReduceOrScan(this, arg0, arg1, arg2);
  return MatrixCommonReduce(this, false, depth, func, mode);
}

function MatrixCommonScan(parexec, self, depth, func, mode) {
  // FIXME(bug 844887): Check |self instanceof Matrix|
  // FIXME(bug 844887): Check |IsCallable(func)|

  var length = self.shape[0];

  if (length === 0)
    ThrowError(JSMSG_PAR_ARRAY_REDUCE_EMPTY);

  var buffer = NewDenseArray(length);
  scan(self.get(0), 0, length);
  return NewMatrix(MatrixView, [length], buffer, 0);

  function scan(accumulator, start, end) {
    UnsafeSetElement(buffer, start, accumulator);
    for (var i = start + 1; i < end; i++) {
      accumulator = func(accumulator, self.get(i));
      UnsafeSetElement(buffer, i, accumulator);
    }
    return accumulator;
  }

}

function MatrixPScan(arg0, arg1, arg2) {
  var [depth, func, mode] =
    MatrixDecomposeArgsForReduceOrScan(this, arg0, arg1, arg2);
  return MatrixCommonScan(true, this, depth, func, mode);
}

function MatrixScan(arg0, arg1, arg2) {
  var [depth, func, mode] =
    MatrixDecomposeArgsForReduceOrScan(this, arg0, arg1, arg2);
  return MatrixCommonScan(false, this, depth, func, mode);
}

function MatrixScatter(targets, defaultValue, conflictFunc, length, mode) {
  ThrowError(JSMSG_BAD_BYTECODE, "Matrix.scatter");
}
function MatrixPScatter(targets, defaultValue, conflictFunc, length, mode) {
  ThrowError(JSMSG_BAD_BYTECODE, "Matrix.pscatter");
}

/**
 * The familiar filter() operation applied across the outermost
 * dimension.
 */
function MatrixCommonFilter(self, parexec, func, mode) {
  // FIXME(bug 844887): Check |this instanceof ParallelArray|
  // FIXME(bug 844887): Check |IsCallable(func)|

  mode && mode.print && mode.print({called:"MCF A"});

  var length = self.shape[0];
  var grain_len = ProductOfArrayRange(self.shape, 1, self.shape.length);

  // The strategy we use for ParallelArrayFilter is
  // specialized around setting a single element at a time.
  // UnsafeSetElement cannot express an atomic combination of:
  // - Copying a whole n-length substring
  // - updating counts
  // - updating info
  // (It could be that such level of generality is not necessary, but
  //  Felix is just skipping this problem for now.)
  parallel: /* no code yet */

  // Sequential fallback:
  ASSERT_SEQUENTIAL_IS_OK(mode);
  var buffer = make_buffer_from_shape_and_valtype(self.shape, self.valtype);
  var count = 0;
  mode && mode.print && mode.print({called:"MCF T2",count:count,length:length,buffer:buffer,self:self,grain_len:grain_len});
  for (var i = 0; i < length; i++) {
    var elem = self.get(i);
    if (func(elem, i, self)) {
      mode && mode.print && mode.print({called:"MCF U2 AC",to:count*grain_len,fro:i*grain_len});
      UnsafeArrayCopy(buffer, count * grain_len,
                      self.buffer, i * grain_len, grain_len);
      count++;
    }
  }
  mode && mode.print && mode.print({called:"MCF W2",count:count,length:length,buffer:buffer,self:self});
  var shape = [count];
  for (var i = 1; i < self.shape.length; i++)
    ARRAY_PUSH(shape, self.shape[i]);

  mode && mode.print && mode.print({called:"MCF Z2"});
  return NewMatrix(MatrixView, shape, buffer, 0, self.valtype);
}

// FIXME: 868422
// This should be (heavily optimized) intrinsic; (think
// System.arraycopy from Java).
function UnsafeArrayCopy(buffer, bufoffset, subbuffer, suboffset, grain_len) {
  for (var j = 0; j < grain_len; j++) {
    UnsafeSetElement(buffer, bufoffset+j, subbuffer[suboffset+j]);
  }
}

function ArrayCopy(buffer, bufoffset, subbuffer, suboffset, grain_len) {
  for (var j = 0; j < grain_len; j++) {
    buffer[bufoffset+j] = subbuffer[suboffset+j];
  }
}

function MatrixPFilter(func, mode) {
  return MatrixCommonFilter(this, true, func, mode);
}

function MatrixFilter(func, mode) {
  return MatrixCommonFilter(this, false, func, mode);
}

/**
 * Divides the outermost dimension into two dimensions.  Does not copy
 * or affect the underlying data, just how it is divided amongst
 * dimensions.  So if we had a vector with shape [M, N, ...] and you
 * partition with amount=4, you get a [M/4, 4, N, ...] vector.  The
 * outermost dimension must be evenly divisble by amount (e.g. 4 must
 * divide M in the above example).
 */
function MatrixPartition(amount) {
  if (amount >>> 0 !== amount)
    ThrowError(JSMSG_PAR_ARRAY_BAD_ARG, "");

  var length = this.shape[0];
  var partitions = (length / amount) | 0;

  if (partitions * amount !== length)
    ThrowError(JSMSG_PAR_ARRAY_BAD_PARTITION);

  var shape = [partitions, amount];
  for (var i = 1; i < this.shape.length; i++)
      ARRAY_PUSH(shape, this.shape[i]);

  return NewMatrix(MatrixView, shape, this.buffer, this.offset, this.valtype);
}

/**
 * Collapses two outermost dimensions into one.  So if you had
 * a [X, Y, Z ...] matrix, you get a [X*Y, Z ...] matrix.
 */
function MatrixFlatten()  {
  if (this.shape.length < 2)
    ThrowError(JSMSG_PAR_ARRAY_ALREADY_FLAT);

  var shape = [this.shape[0] * this.shape[1]];
  for (var i = 2; i < this.shape.length; i++)
    ARRAY_PUSH(shape, this.shape[i]);
  return NewMatrix(MatrixView, shape, this.buffer, this.offset, this.valtype);
}

function MatrixToString() {
  var self = this;
  var slen = self.shape.length;
  if (slen == 1) {
    var subarray = [].slice.call(this.buffer, self.offset, self.offset+self.shape[0]);
    return "[" + subarray.join(",") + "]";
  }

  var dim0 = self.shape[0];
  var dim1 = self.shape[1];
  var p = false;
  if (slen > 2) {
    p = 1;
    for (var i=2; i < slen; i++) {
      p *= self.shape[i];
    }
  }
  var matrix = self;

  var payload = false;
  var ret = "[";
  var matrixNeedsNewline = false;
  for (var row=0; row < dim0; row++) {
    if (matrixNeedsNewline)
      ret += ",\n ";
    ret += "[";
    var rowNeedsComma = false;
    for (var x=0; x < dim1; x++) {
      if (rowNeedsComma)
        ret += ", ";
      var val = matrix.get(row, x);
      if (IsParallelArray(val)) {
        ret += "<"+val+">";
      } else if (val !== undefined) {
        ret += val;
      }
      rowNeedsComma = true;
    }
    ret += "]";
    matrixNeedsNewline = true;
  }
  ret += "]";
  return ret;
}

/**
 * Internal debugging tool: checks that the given `mode` permits
 * sequential execution
 */
function AssertSequentialIsOK(mode) {
  if (mode && mode.mode && mode.mode !== "seq" && ParallelTestsShouldPass())
    ThrowError(JSMSG_WRONG_VALUE, "parallel execution", "sequential was forced");
}

/**
 * Internal debugging tool: returns a function to be supplied to
 * ParallelDo() that will check that the parallel results
 * bailout/succeed as expected. Returns null if no mode is supplied
 * or we are building with some strange IF_DEF configuration such that
 * we don't expect parallel execution to work.
 */
function CheckParallel(mode) {
  if (!mode || !ParallelTestsShouldPass())
    return null;

  return function(bailouts) {
    if (!("expect" in mode) || mode.expect === "any") {
      return; // Ignore result when unspecified or unimportant.
    }

    var result;
    if (bailouts === 0)
      result = "success";
    else if (bailouts === global.Infinity)
      result = "disqualified";
    else
      result = "bailout";

    if (mode.expect === "mixed") {
      if (result === "disqualified")
        ThrowError(JSMSG_WRONG_VALUE, mode.expect, result);
    } else if (result !== mode.expect) {
      ThrowError(JSMSG_WRONG_VALUE, mode.expect, result);
    }
  };
}

/*
 * Mark the main operations as clone-at-callsite for better precision.
 * This is slightly overkill, as all that we really need is to
 * specialize to the receiver and the elemental function, but in
 * practice this is likely not so different, since element functions
 * are often used in exactly one place.
 */
SetScriptHints(ParallelArrayConstructEmpty, { cloneAtCallsite: true });
SetScriptHints(ParallelArrayConstructFromArray, { cloneAtCallsite: true });
SetScriptHints(ParallelArrayConstructFromFunction, { cloneAtCallsite: true });
SetScriptHints(ParallelArrayConstructFromFunctionMode, { cloneAtCallsite: true });
SetScriptHints(ParallelArrayConstructFromComprehension, { cloneAtCallsite: true });
SetScriptHints(ParallelArrayView,       { cloneAtCallsite: true });
SetScriptHints(ParallelArrayBuild,      { cloneAtCallsite: true });
SetScriptHints(ParallelArrayMap,        { cloneAtCallsite: true });
SetScriptHints(ParallelArrayReduce,     { cloneAtCallsite: true });
SetScriptHints(ParallelArrayScan,       { cloneAtCallsite: true });
SetScriptHints(ParallelArrayScatter,    { cloneAtCallsite: true });
SetScriptHints(ParallelArrayFilter,     { cloneAtCallsite: true });

SetScriptHints(UnsafeArrayCopy,       { cloneAtCallsite: true });

/*
 * Mark the common getters as clone-at-callsite and inline. This is
 * overkill as we should only clone per receiver, but we have no
 * mechanism for that right now. Bug 804767 might permit another
 * alternative by specializing the inlined gets.
 */
SetScriptHints(ParallelArrayGet1,       { cloneAtCallsite: true, inline: true });
SetScriptHints(ParallelArrayGet2,       { cloneAtCallsite: true, inline: true });
SetScriptHints(ParallelArrayGet3,       { cloneAtCallsite: true, inline: true });

SetScriptHints(MatrixGet1,      { cloneAtCallsite: true, inline: true });
SetScriptHints(MatrixGet2,      { cloneAtCallsite: true, inline: true });
SetScriptHints(MatrixGet3,      { cloneAtCallsite: true, inline: true });

SetScriptHints(MatrixConstructFromGrainFunctionMode, { cloneAtCallsite: true });
SetScriptHints(MatrixPMap,                           { cloneAtCallsite: true });
SetScriptHints(MatrixCommonMap,                      { cloneAtCallsite: true });
SetScriptHints(MatrixDecomposeArgsForMap,            { cloneAtCallsite: true });
SetScriptHints(MatrixPReduce,                        { cloneAtCallsite: true });
