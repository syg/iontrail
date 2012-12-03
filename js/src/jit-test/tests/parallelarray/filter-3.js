load(libdir + "parallelarray-helpers.js");

function testFilterSome() {
  // Test filtering out indices 3^n for all n:
  function test(e, i) {
    return (i % 3) != 0;
  }
  var buffer = [], keep = [];
  for (var i = 0; i < 1024; i++) {
    buffer[i] = i;
    keep[i] = test(buffer[i], i);
  }
  var expected = buffer.filter(test);

  var pa = new ParallelArray(buffer);
  assertParallelArrayModesEq(["seq", "par"], expected, function(m) {
    return pa.filter(keep, m);
  });
}

testFilterSome();
