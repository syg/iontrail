// |jit-test| error: Error;

load(libdir + "parallelarray-helpers.js");

function testMap() {
  // Should not fail parallel execution
  var p = new ParallelArray([0,1,2,3,4]);
  var m = p.map(function (v) { return v+1; }, { mode: "par", expect: "fail" });
  assertEqParallelArray(m, new ParallelArray([1,2,3,4,5]));
}

testMap();

