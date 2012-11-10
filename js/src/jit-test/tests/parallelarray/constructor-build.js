load(libdir + "parallelarray-helpers.js");

function buildSimple() {

    /*
    new ParallelArray([256], function(i) {
        var x = [];
        for (var i = 0; i < 4; i++) {
            x[i] = i;
        }
        return x;
    }, {mode: "par", expect: "success"});
    */

    // Eventually, this should work, but right now it
    // bails out because we overflow the size of the array
    new ParallelArray([256], function(i) {
        var x = [];
        for (var i = 0; i < 99; i++) {
            x[i] = i;
        }
        return x;
    }, {mode: "par", expect: "bail"});

//    assertParallelArrayModesCommute(["seq", "par"], function(m) {
//        new ParallelArray([256], function(i) {
//            var x = [];
//            for (var i = 0; i < 99; i++) {
//                x[i] = i;
//            }
//            return x;
//        }, m);
//    });
    
}

buildSimple();
