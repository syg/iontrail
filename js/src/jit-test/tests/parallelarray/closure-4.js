function testClosureCreationAndInvocation() {
    var a = [1,2,3,4,5];
    var p = new ParallelArray(a);
    function makeaddv(v) { return function (x) { return x+v; }; };
    // eta-expansion is (or at least can be) treated as call with unknown target
    var m = p.map(makeaddv, {mode: "par", expect: "success"});
    assertEq(m[1](1), 3); // (\x.x+v){v=2} 1 == 3
    assertEq(m[2](2), 5); // (\x.x+v){v=3} 2 == 5
}

testClosureCreationAndInvocation();
