// Adapted from
//
// https://github.com/RiverTrail/RiverTrail/blob/master/examples/mandelbrot/mandelbrot.js
//
// which in turn is adapted from a WebCL implementation available at
//
// http://www.ibiblio.org/e-notes/webcl/mandelbrot.html

var nc = 30, maxCol = nc*3, cr,cg,cb;

load(libdir + "util.js");

function buildBalancedTree(depth) {
  if (depth == 0) {
    return [];
  } else {
    return [buildBalancedTree(depth-1), buildBalancedTree(depth-1)];
  }
}

function computeSequentially() {
  result = [];
  for (var r = 0; r < ROWS; r++) {
    for (var c = 0; c < COLS; c++) {
      result.push(buildBalancedTree(DEPTH));
    }
  }
  return result;
}

function computeParallel() {
  return new ParallelArray([ROWS, COLS], function(r, c) {
    return buildBalancedTree(DEPTH);
  }).flatten();
}

var ROWS  = 1024;
var COLS  = 512;
var DEPTH = 2;

// Experimentally, warmup doesn't seem to be necessary:
benchmark("ALLOCATOR", 1, DEFAULT_MEASURE,
          computeSequentially, computeParallel);
