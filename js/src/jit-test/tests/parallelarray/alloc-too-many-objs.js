load(libdir + "parallelarray-helpers.js");

function testMap() {

  // Note: This is the same kernel function as `alloc-many-objs`, but
  // with a larger bound.  This often fails par. exec. because it
  // triggers GC at inconvenient times.  But let's just test that it
  // doesn't crash or something!

  var ints = range(0, 10);
  var pints = new ParallelArray(ints);
  var presult = pints.map(kernel);
  var sresult = ints.map(kernel);
  assertStructuralEq(sresult, presult);

  function kernel(v) {
    var x = [];
    for (var i = 0; i < 500000; i++) {
      x[i] = {from: v};
    }
    return x;
  }
}

// FIXME we do not squelch GC properly in this case
// testMap();

