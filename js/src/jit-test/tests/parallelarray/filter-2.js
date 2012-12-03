load(libdir + "parallelarray-helpers.js");

function testFilterNone() {
  // Test filtering everything out
  var buffer = [], falses = [];
  for (var i = 0; i < 1024; i++) {
    buffer[i] = i;
    falses[i] = false;
  }
  
  var p = new ParallelArray(buffer);
  var r = p.filter(falses);
  assertStructuralEq(r, []);
}

testFilterNone();
