load(libdir + "parallelarray-helpers.js");

function cell(...args) {
  var ret = 0;
  var d;
  while ((d = args.shift()) != undefined) {
    ret *= 10;
    ret += d+1;
  }
  return ret;
}

function test_2d() {
  // An omitted grain is synonymous with a grain of ["any"]
  var pm2d_1 =  new ParallelMatrix([5,6], cell);

  var pm2d_2 =  new ParallelMatrix([5,6], ["any"], cell);

  var pm2d_3 = new ParallelMatrix([5], [6],
    function(i) {
      return new ParallelMatrix([6], ["any"], function (j) cell(i,j));
    });

  var pm2d_4 = new ParallelMatrix([5], [6],
    function(i) {
      return new ParallelMatrix([6], function (j) cell(i,j));
    });

  var pm2d_5 =  new ParallelMatrix([5,6], ["uint32"], cell);

  assertEqParallelMatrix(pm2d_1, pm2d_2);
  assertEqParallelMatrix(pm2d_1, pm2d_3);
  assertEqParallelMatrix(pm2d_1, pm2d_4);
  assertEqParallelMatrix(pm2d_1, pm2d_5);
}

test_2d();
