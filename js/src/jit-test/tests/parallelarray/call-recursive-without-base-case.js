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
    var p = new ParallelArray([0,1,2,3,4,5,22,6,7,8]);
    var q = p.map(function(x) { return factorial(x); },
                  { mode: "par", expect: "bail" });
}

testMap();

