// |jit-test| error: expected success but found bailout

load(libdir + "parallelarray-helpers.js");

function test() {
  var x = new Error();
  function inc(n) {
    if (n == 33) // doesn't occur during warmup
      throw x;
    return n + 1;
  }
  var x = new ParallelArray(range(0, 512));

  // Note: in fact, a bailout occurs due to throwing an exception.
  x.map(inc, {mode: "par", expect: "success"});
}
test();

