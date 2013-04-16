// Adapted from ZPL code for Jacobi Iteration by Lawrence Snyder
// ("A Programmers Guide to ZPL", pg 3, 1999)

var n     = 512;
var delta = 0.000001;

load(libdir + "util.js");

var A = new ParallelMatrix([n,n], ["float64"], (i,j) => 0.0);

function kernel(i,j) {
  // south border is all 1.0; other borders are 0.0.
  var a_north = (i > 0) ? A.get(i-1, j)   : 0.0;
  var a_west  = (j > 0) ? A.get(i, j-1)   : 0.0;
  var a_east  = (j < n-1) ? A.get(i, j+1) : 0.0;
  var a_south = (i < n-1) ? A.get(i+1, j) : 1.0;

  return (a_north + a_west + a_east + a_south) / 4.0;
}

function jacobi() {
  do {
    var Temp = new ParallelMatrix([n,n], ["float64"], kernel);
    var change = new ParallelMatrix([n,n], ["float64"],
        function (i,j) {
          return Math.abs(A.get(i,j) - Temp.get(i,j));
        });
    var err = change.reduce( (a,b) => Math.max(a,b) );
    A = Temp;
  } while (err >= delta);
}
