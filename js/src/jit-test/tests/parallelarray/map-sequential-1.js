
load(libdir + "parallelarray-helpers.js");

function testMap() {
  // Should not fail parallel execution
  var p = new ParallelArray(range(0, 64));
  var m = p.map(function (v) { return v+1; }, { mode: "par", expect: "fail" });
  assertEqParallelArray(m, new ParallelArray(range(1, 64)));
}

// FIXME---mode assertions not impl in self-hosted code
// |jit-test| error: Error;
// testMap();

