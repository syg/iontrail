load(libdir + "parallelarray-helpers.js");

var SIZE = 4096;

function testMap() {
  function inc(e) { return e+1; }

  var array = [];
  for (var i = 0; i <= SIZE; i++) array.push(i);

  var expected = array.map(inc);

  var parray = new ParallelArray(array);
  assertParallelArrayModesEq(["seq", "par"], expected, function(m) {
    d = parray.map(inc, m);
    return d;
  });
}

testMap();
