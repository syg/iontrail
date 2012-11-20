load(libdir + "parallelarray-helpers.js");

function testReduceOne() {
    var array = [];
    for (var i = 1; i < 1024; i++) { array.push(i); }

    var pa = new ParallelArray(array);

    assertParallelArrayModesCommute(["seq", "par"], function(m) {
        return pa.reduce(function (v, p) { return v + p; }, m);
    });
}

testReduceOne();
