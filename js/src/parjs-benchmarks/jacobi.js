// Adapted from ZPL code for Jacobi Iteration by Lawrence Snyder
// ("A Programmers Guide to ZPL", pg 3, 1999)

var n     = 512;
var delta = 0.000001;

load(libdir + "util.js");

var A = new Matrix([n,n], ["float64"], (i,j) => 0.0);

function kernel(i,j) {
  // south border is all 1.0; other borders are 0.0.
  var a_north = (i > 0) ? A.get(i-1, j)   : 0.0;
  var a_west  = (j > 0) ? A.get(i, j-1)   : 0.0;
  var a_east  = (j < n-1) ? A.get(i, j+1) : 0.0;
  var a_south = (i < n-1) ? A.get(i+1, j) : 1.0;

  return (a_north + a_west + a_east + a_south) / 4.0;
}

function jacobi_par() {
  do {
    var Temp = new Matrix([n,n], ["float64"], kernel);
    var change = new Matrix([n,n], ["float64"],
        function (i,j) {
          return Math.abs(A.get(i,j) - Temp.get(i,j));
        });
    var err = change.reduce( (a,b) => Math.max(a,b) );
    A = Temp;
  } while (err >= delta);
}

function jacobi_seq() {
  // Ideally this would be written with bummed imperative code rather
  // than using the mode parameter to force sequential execution.
  // But I do not want to take the time to produce that ideal solution.
  do {
    var mode = {mode:"seq"};
    var Temp = new Matrix([n,n], ["float64"], kernel, mode);
    var change = new Matrix([n,n], ["float64"],
        function (i,j) {
          return Math.abs(A.get(i,j) - Temp.get(i,j));
        }, mode);
    var err = change.reduce( (a,b) => Math.max(a,b), mode);
    A = Temp;
  } while (err >= delta);
}

// temporarily reduce input size so that we can actually finish
// a warmup run in a reasonable amount of time.
n = 16;
benchmark("JACOBI-16", DEFAULT_WARMUP, DEFAULT_MEASURE,
          jacobi_seq, jacobi_par);
n = 512;
benchmark("JACOBI-512", DEFAULT_WARMUP, DEFAULT_MEASURE,
          jacobi_seq, jacobi_par);
