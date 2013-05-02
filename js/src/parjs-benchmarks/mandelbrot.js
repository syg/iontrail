// Adapted from
//
// https://github.com/RiverTrail/RiverTrail/blob/master/examples/mandelbrot/mandelbrot.js
//
// which in turn is adapted from a WebCL implementation available at
//
// http://www.ibiblio.org/e-notes/webcl/mandelbrot.html

var nc = 30, maxCol = nc*3, cr,cg,cb;

load(libdir + "util.js");

// this is the actual mandelbrot computation, ported to JavaScript
// from the WebCL / OpenCL example at
// http://www.ibiblio.org/e-notes/webcl/mandelbrot.html
function computeSetByRow(x, y) {
  var Cr = (x - 256) / scale + 0.407476;
  var Ci = (y - 256) / scale + 0.234204;
  var I = 0, R = 0, I2 = 0, R2 = 0;
  var n = 0;
  while ((R2+I2 < 2.0) && (n < 512)) {
    I = (R+R)*I+Ci;
    R = R2-I2+Cr;
    R2 = R*R;
    I2 = I*I;
    n++;
  }
  return n;
}

function computeSequentially() {
  result = [];
  for (var r = 0; r < rows; r++) {
    for (var c = 0; c < cols; c++) {
      result.push(computeSetByRow(c, r));
    }
  }
  return result;
}

function computeSequentiallyInt16() {
  var buf = new ArrayBuffer(rows*cols*2);
  result = new Int16Array(buf);
  for (var r = 0; r < rows; r++) {
    for (var c = 0; c < cols; c++) {
      result[ r*cols + c ] = computeSetByRow(c, r);
    }
  }
  return result;
}

function computeParallel() {
  return new ParallelArray([rows, cols], function(r, c) {
    return computeSetByRow(c, r);
  }).flatten();
}

function computeParallelInt16() {
  return new ParallelMatrix([rows, cols], ["int16"], function(r, c) {
    return computeSetByRow(c, r);
  }).flatten();
}

function compare(arrs, pas) {
  for (var r = 0; r < rows; r++) {
    for (var c = 0; c < cols; c++) {
      assertEq(seq[c + r * cols], par.get(r, c));
    }
  }
}

var scale = 10000*300;
var rows = 1024;
var cols = 1024;

// Experimentally, warmup doesn't seem to be necessary:
benchmark("MANDELBROT ARRAY", 1, DEFAULT_MEASURE,
          computeSequentially, computeParallel);

benchmark("MANDELBROT M I16", 1, DEFAULT_MEASURE,
          computeSequentiallyInt16, computeParallelInt16);
