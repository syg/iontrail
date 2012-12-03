load(libdir + "parallelarray-helpers.js");

function testReduce() {
  function sum(v, p) { return v*p; }

  var array = [];
  for (var i = 1; i <= 512; i++) array.push((i % 4) + 1);
  var expected = array.reduce(sum);
  print(expected);

  var parray = new ParallelArray(array);
  assertParallelArrayModesEq(["seq", "par"], expected, function(m) {
    d = parray.reduce(sum, m);
    return d;
  }, assertAlmostEq);
}

testReduce();
