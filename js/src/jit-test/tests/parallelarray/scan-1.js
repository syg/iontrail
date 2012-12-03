load(libdir + "parallelarray-helpers.js");

function testScan() {
  function sum(a, b) { return a+b; }

  function seq_scan(array, f) {
    var result = [];
    result[0] = array[0];
    for (var i = 1; i < array.length; i++) {
      result[i] = f(result[i-1], array[i]);
    }
    return result;
  }

  var array = [];
  for (var i = 1; i <= 64; i++) array.push(i);
  var expected = seq_scan(array, sum);

  var parray = new ParallelArray(array);
  
  // Lame: par mode needs a few runs to get up to speed.
  for (var i = 0; i < 10; i++) {
    var r = parray.scan(sum);
    assertStructuralEq(r, expected);
  }

  /*
  assertParallelArrayModesEq(["seq", "par"], expected, function(m) {
    x = parray.scan(sum, m);
    print(x.toString());
    return x;
  });
  */
}

testScan();
