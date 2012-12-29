// |jit-test| error: expected success but found disqualified

load(libdir + "parallelarray-helpers.js");

function test() {
  var x = new Error();
  function inc(n) {
    if (inParallelSection()) // wait until par execution, then throw
      throw x;
    return n + 1;
  }
  var x = new ParallelArray(range(0, 2048));

  // Note: in fact, a bailout occurs due to throwing an exception.
  x.map(inc, {mode: "par", expect: "success"});
}
test();

