load(libdir + "parallelarray-helpers.js");

function testFilterAll() {
  // Test filtering everything (leaving everything in)
  var buffer = [], trues = [];
  for (var i = 0; i < 1024; i++) {
    buffer[i] = i;
    trues[i] = true;
  }
  
  var p = new ParallelArray(buffer);
  var r = p.filter(trues);
  assertEqParallelArray(r, p);

//  var p = new ParallelArray([1024, 4], function(i,j) { return i*10000 + j; });
//  var r = p.filter(trues);
//  assertEqParallelArray(r, p);
}

testFilterAll();
