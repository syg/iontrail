load(libdir + "parallelarray-helpers.js");

function test() {
  var x = new Error();
  function inc(n) {
    if (n == 5) // doesn't occur during warmup
      throw x;
    return n + 1;
  }
  var x = new ParallelArray(range(0, 512));
  x.map(inc, {mode: "par", expect: "bailout"});
}
test();

