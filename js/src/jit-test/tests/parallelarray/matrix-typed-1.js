load(libdir + "parallelarray-helpers.js");

function cell(...args) {
  var ret = 0;
  var d;
  while ((d = args.shift()) != undefined && typeof d === "number") {
    ret *= 10;
    ret += d+1;
  }
  return ret;
}

function test_2d() {
  // An omitted grain is synonymous with a grain of ["any"]
  var pm2d_1 =  new Matrix([5,6], cell);

  var pm2d_2 =  new Matrix([5,6], ["any"], cell);

  var pm2d_3 = new Matrix([5], [6],
    function(i) {
      return new Matrix([6], ["any"], function (j) cell(i,j));
    });

  var pm2d_4 = new Matrix([5], [6],
    function(i) {
      return new Matrix([6], function (j) cell(i,j));
    });

  var pm2d_5 =  new Matrix([5,6], ["uint32"], cell);

  assertEqMatrix(pm2d_1, pm2d_2);
  assertEqMatrix(pm2d_1, pm2d_3);
  assertEqMatrix(pm2d_1, pm2d_4);
  assertEqMatrix(pm2d_1, pm2d_5);
}

test_2d();
