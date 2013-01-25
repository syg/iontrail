// This file expects the lib/parallelarray-helpers.js file
// to already be loaded when you run it.
// I.e. when using js shell, run in this fashion:
//
// js -f path/to/parallelarray-helpers.js -f path/to/this-file.js

// Adapted from
//
// https://github.com/RiverTrail/RiverTrail/blob/master/examples/mandelbrot/mandelbrot.js
//
// which in turn is adapted from a WebCL implementation available at
//
// http://www.ibiblio.org/e-notes/webcl/mandelbrot.html

var nc = 30, maxCol = nc*3;

// In practice these arrays get initialized into a color map, but
// that is not necessary for our purposes here (the benchmark will
// not use the results of the array lookups).
var cr = new Array(maxCol+1),cg = new Array(maxCol+1),cb=new Array(maxCol+1);

function iterationCountToColor(n) {
    var ci = 0;
    if (n == 512) {
        ci = maxCol;
    } else {
        ci = n % maxCol;
    }
    return [cr[ci],cg[ci],cb[ci],255];
}

// this is the actual mandelbrot computation, ported to JavaScript
// from the WebCL / OpenCL example at
// http://www.ibiblio.org/e-notes/webcl/mandelbrot.html
function computeSetByRow(y, x) {
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
  // return n;
  return iterationCountToColor(n);
}

function computeSequentially() {
  result = [];
  for (var r = 0; r < rows; r++) {
    for (var c = 0; c < cols; c++) {
      result.push(computeSetByRow(r, c));
    }
  }
  return result;
}

var scale = 10000*300;
var rows = 512;
var cols = 512;

var goWithVerify = (function () {
            var reference = computeSequentially();
            var i = 0;
            return function () {
              i++;
              print("mandel "+i);
              var r = new ParallelArray([rows, cols], computeSetByRow);
              var m = r.flatten();
              assertStructuralEq(m, reference);
              return m;
            };})();

var go =
  (function () {
     var i = 0;
     return function() {
       i++;
       print("doing mbrot "+i);
       var mandelbrot = new ParallelArray([512,512], computeSetByRow);
     };})();

function repeat(i,body) { while (i > 0) { body(); i--; } }

repeat(30, go);
