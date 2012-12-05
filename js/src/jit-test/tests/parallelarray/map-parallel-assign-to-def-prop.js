function testMap() {
    var p = new ParallelArray([0,1,2,3,4]);
    var func = function (v) {
        var obj = {f: 2};
        obj.f += v;
        return obj;
    };
    var q = p.map(func, { mode: "par", expect: "success" });
    for (var i = 0; i < 5; i++) {
        if (q.get(i).f != p.get(i) + 2)
            throw new Exception("Index " + i + " is wrong: " +
                                q.get(i).f + " vs " + (p.get(i) + 2));
    }
}

testMap();

