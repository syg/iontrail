// Adapted from
//
// https://github.com/RiverTrail/RiverTrail/blob/master/examples/mandelbrot/mandelbrot.js
//
// which in turn is adapted from a WebCL implementation available at
//
// http://www.ibiblio.org/e-notes/webcl/mandelbrot.html

load(libdir + "parallelarray-helpers.js");

var nc = 30, maxCol = nc*3, cr,cg,cb;

// initialises the color map for translating Mandelbrot iterations
// into nice colors
function computeColorMap() {
   var st = 255/nc;
   cr = new Array(maxCol); cg = new Array(maxCol); cb = new Array(maxCol);
   for (var i = 0; i < nc; i++){
     var d = Math.floor(st*i);
     cr[i] = 255 - d; cr[i+nc] = 0; cr[i+2*nc] = d;
     cg[i] = d; cg[i+nc] = 255 - d; cg[i+2*nc] = 0;
     cb[i] = 0; cb[i+nc] = d; cb[i+2*nc] = 255 - d;
   }
   cr[maxCol] = cg[maxCol] = cb[maxCol] = 0;
}

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
        column = [];
        for (var c = 0; c < cols; c++) {
            column.push(computeSetByRow(r, c));
        }
        result.push(column);
    }
    return result;
}

var scale = 10000*300;
// var rows = 2048;
// var cols = 2048;
var rows = 1024;
var cols = 1024;

computeColorMap();

// run once to get jit up and going
// var expected = computeSequentially();

// check that we get correct result
assertParallelArrayModesCommute(["seq", "par"], function(m) {
  r = new ParallelArray([rows, cols], computeSetByRow);
  return r;
});

// compare performance
/*
comparePerformance([
    {name: "seq",
     func: computeSequentially},
    {name: "par",
     func: function() { new ParallelArray([rows, cols], computeSetByRow) }}]);
*/
