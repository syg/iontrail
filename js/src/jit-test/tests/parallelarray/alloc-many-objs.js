function testMap() {
    var p = new ParallelArray([0,1,2,3,4]);
    var func = function (v) {
        var x;
        for (var i = 0; i < 1000000; i++) {
            x = {f: v};
        }
        return x;
    };
    var q = p.map(func, { mode: "par", expect: "success" });
}

testMap();

