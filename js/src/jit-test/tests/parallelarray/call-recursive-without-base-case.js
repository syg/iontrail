// |jit-test| exitstatus: 3;

function factorial(n) {
    if (n > 10)
        return factorial(n);

    // Note: no base case :)
    if (n == 0)
        return 1;
    return n * factorial(n - 1);
}

function testMap() {
    // sneak in a 22 that causes infinite iteration, but only
    // after warmup iters have completed!
  var p = new ParallelArray([0,1,2,3,4,5,22,7,8,
                             9,10,11,12,13,14,15,
                             16,17,18,19,20,21,22,23,
                             24,25,26,27,28,29,30,31]);
    var q = p.map(function(x) { return factorial(x); },
                  { mode: "par", expect: "bail" });
}

testMap();

