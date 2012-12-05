// |jit-test| exitstatus: 3;

function kernel(n) {
  if (n > 10)
      return kernel(n);

  // Note: no base case :)
  return n+1;
}

function testMap() {
  // sneak in a 22 that causes infinite iteration, but only
  // after warmup iters have completed!
  var r = [];
  for (var i = 0; i < 1024; i++) r[i] = i % 9;
  r[22] = 22;
  var p = new ParallelArray(r);
    var q = p.map(function(x) { return kernel(x); },
                  { mode: "par", expect: "bail" });
}

testMap();

