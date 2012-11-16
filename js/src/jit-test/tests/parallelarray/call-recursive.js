function factorial(n) {
    if (n == 0)
        return 1;
    return n * factorial(n - 1);
}

function testMap() {
    var p = new ParallelArray([0,1,2,3,4]);
    var q = p.map(function(x) { return factorial(x); }, { mode: "par", expect: "success" });
    for (var i = 0; i < 5; i++) {
        if (q[i] != factorial(p[i]))
            throw new Exception("Index " + i + " is wrong: " +
                                q[i] + " vs " + factorial(p[i]));
    }
}

testMap();

