load(libdir + "parallelarray-helpers.js");

function testMap() {
    var q = {f: 1};
    var p = new ParallelArray([0,1,2,3,4]);
    var func = function (v) { return v+q.f; };
    var m = p.map(func, { mode: "par", expect: "success" });
    assertEqParallelArray(m, new ParallelArray([1,2,3,4,5]));
}

testMap();

