function kernel(n) {
  if (n > 10)
    // Note: no base case :)
    return kernel(n);
  return n+1;
}

function testMap() {
  // sneak in a 22 that causes infinite iteration, but only
  // after warmup iters have completed!
  var r = [];
  for (var i = 0; i < 1024; i++) r[i] = i % 9;
  r[762] = 22;
  var p = new ParallelArray(r);
  p.map(kernel, { mode: "par", expect: "bailout" });
}

testMap();

