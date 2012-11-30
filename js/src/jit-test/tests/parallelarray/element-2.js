load(libdir + "parallelarray-helpers.js");

function testElement() {
  // Test getting element from higher dimension
  var p = new ParallelArray([2,2,2], function () { return 0; });
  var p0 = new ParallelArray([2,2], function () { return 0; });
  assertEqParallelArray(p.get(0), p0);
  // Should create new wrapper
  assertEq(p.get(0) !== p.get(0), true);
  // Test out of bounds
  assertEq(p.get(42), undefined);
  // Test getting element from 0-lengthed higher dimension
  var pp = new ParallelArray([0,0], function() { return 0; });
  assertEq(pp.get(2), undefined);
  var pp2 = new ParallelArray([2,0], function() { return 0; });
  assertEqParallelArray(pp2.get(0), new ParallelArray());
}

testElement();
