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
  var pm2d_1 =  new ParallelMatrix([5,6], cell);

  // An grain of length 0 is synonymous with an omitted grain argument.
  var pm2d_2 =  new ParallelMatrix([5,6], [], cell);

  var pm2d_3 = new ParallelMatrix([5], [6],
    function(i) {
      return new ParallelMatrix([6], [], function (j) cell(i,j));
    });

  var pm2d_4 = new ParallelMatrix([5], [6],
    function(i) {
      return new ParallelMatrix([6], function (j) cell(i,j));
    });

  assertEqParallelMatrix(pm2d_1, pm2d_2);
  assertEqParallelMatrix(pm2d_1, pm2d_3);
  assertEqParallelMatrix(pm2d_1, pm2d_4);
}

function test_3d() {
  var pm3d_1 = new ParallelMatrix([5,6,7], cell);

  var pm3d_2 = new ParallelMatrix([5,6], [7],
    function(i,j) {
      return new ParallelMatrix([7], function (k) cell(i,j,k)); });

  var pm3d_3 = new ParallelMatrix([5], [6,7],
    function(i) {
      return new ParallelMatrix([6,7], function (j,k) cell(i,j,k));
    });

  var pm3d_4 = new ParallelMatrix([5], [6,7],
    function(i) {
      return new ParallelMatrix([6], [7],
        function (j) {
          return new ParallelMatrix([7], function (k) cell(i,j,k)); }); });

  assertEqParallelMatrix(pm3d_1, pm3d_2);
  assertEqParallelMatrix(pm3d_1, pm3d_3);
  assertEqParallelMatrix(pm3d_1, pm3d_4);
}

function test_6d() {
  // The elements in overall_6d below is trying to hit sweet spot that has:
  //   (1) a "small" element count, to keep running time manageable,
  //   (2) non-trivial range in each dimenension.
  //
  // At one point, it also tried to provide:
  //   (3) easy-to-distinguish substrings by eye.
  // but since the subranges are now derived from overall_6d via slice,
  // this third goal is unimportant.
  var overall_6d = [3,3,3,3,3,3];

  var pm6d_1 = new ParallelMatrix(overall_6d, cell);

  var pm6d_2 = new ParallelMatrix(overall_6d.slice(0,2), overall_6d.slice(2),
    function(i,j) {
      return new ParallelMatrix(overall_6d.slice(2),
                                function (k,l,m,n) cell(i,j,k,l,m,n)); });

  var pm6d_3 = new ParallelMatrix(overall_6d.slice(0,2), overall_6d.slice(2),
    function(i,j) {
      return new ParallelMatrix(overall_6d.slice(2,3), overall_6d.slice(3),
        function (k) {
          return new ParallelMatrix(overall_6d.slice(3),
                                    function(l,m,n) cell(i,j,k,l,m,n));
        });
    });

  assertEqParallelMatrix(pm6d_1, pm6d_2);
  assertEqParallelMatrix(pm6d_1, pm6d_3);
}

test_2d();
test_3d();
test_6d();
