load(libdir + "parallelarray-helpers.js");

function testMap() {
    var nums = [];
    for (var i = 0; i < 1000; i++) {
        nums[i] = i;
    }
    nums = new ParallelArray([nums]);

    assertParallelArrayModesCommute(["seq", "par"], function(m) {
        nums.map(function (v) {
            var x = [];
            for (var i = 0; i < 500000; i++) {
                x[i] = {from: v};
            }
            return x;
        }, m)
    });
}

testMap();

