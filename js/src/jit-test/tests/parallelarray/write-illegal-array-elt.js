function testMap() {
    var p = new ParallelArray([0,1,2,3,4]);
    var v = [1];
    var func = function (e) {
        v[0] = e;
        return 0;
    };

    // this will compile, but fail at runtime
    p.map(func, {"mode": "par", "expect": "bail"});
}

testMap();

