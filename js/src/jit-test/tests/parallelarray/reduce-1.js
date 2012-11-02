load(libdir + "parallelarray-helpers.js");

function testReduce() {
    function mul(v, p) { return v*p; }

    var array = [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19];
    var expected = array.reduce(mul);

    var parray = new ParallelArray(array);
    assertParallelArrayModesEq(["seq", "par"], expected, function(m) {
        return parray.reduce(mul, m);
    });
}

testReduce();
