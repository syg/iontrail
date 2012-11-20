load(libdir + "parallelarray-helpers.js");

function testReduce() {
    function mul(v, p) { return v*p; }

    var array = [];
    for (var i = 1; i <= 1024; i++) array.push(i);
    var expected = array.reduce(mul);

    var parray = new ParallelArray(array);
    assertParallelArrayModesEq(["seq", "par"], expected, function(m) {
        return parray.reduce(mul, m);
    });
}

testReduce();
