load(libdir + "parallelarray-helpers.js");

function cell(i,j,k) {
  return (i+1)*1000+j*100+k*10;
}

function test_2d() {
  var pm1 = new ParallelMatrix([4, 3], [   4,"uint8"],
    function(i,j) { return new ParallelMatrix([   4], ["uint8"],
      function (   k) { return cell(i, j, k); }); });

  var pm2 = new ParallelMatrix([4   ], [3, 4,"uint8"],
    function(i  ) { return new ParallelMatrix([3, 4], ["uint8"],
      function (j, k) { return cell(i, j, k); }); });

  assertEqParallelMatrix(pm1, pm2);
}

test_2d();
