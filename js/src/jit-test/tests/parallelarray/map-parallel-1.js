load(libdir + "parallelarray-helpers.js");

function testMap() {
  function f(v) { return v+1; }
  var p = new ParallelArray([0,1,2,3,4]);
  var m = p.map(f, { mode: "par", expect: "success" });
  assertEqParallelArray(m, new ParallelArray([1,2,3,4,5]));
  assertParallelArrayModesCommute(["seq", "par"], function(m) {
      return p.map(f, m);
  });
}

testMap();

