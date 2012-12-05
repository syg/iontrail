load(libdir + "parallelarray-helpers.js");

function testReduce() {
  // This test is interesting because during warmup v*p remains an
  // integer but this ceases to be true once real execution proceeds.
  // By the end, it will just be some double value.

  function mul(v, p) { return v*p; }

  // Ensure that the array only contains values between 1 and 4.
  var array = range(1, 513).map(function(v) { return (v % 4) + 1; });
  var expected = array.reduce(mul);
  print(expected);

  var parray = new ParallelArray(array);
  var modes = ["par", "par"];
  for (var i = 0; i < 2; i++) {
    assertAlmostEq(expected, parray.reduce(mul, {mode: modes[i], expect: "success"}));
  }
  // compareAgainstArray(array, "reduce", mul, assertAlmostEq);
}

testReduce();
