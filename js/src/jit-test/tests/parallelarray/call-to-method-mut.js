function MyClass(a) {
    this.a = a;
}

MyClass.prototype.getA = function() {
    return this.a;
}

function testMap() {
    // first time, everything goes well.
    var p = new ParallelArray([new MyClass(22), new MyClass(23)]);
    var q = p.map(function(e) { return e.getA(); },
                  {mode: "par", expect: "success"});
    print(q);
    assertEq(q[0], 22);
    assertEq(q[1], 23);

    // second time, we mutate getA().  This should trigger a
    // recompilation which will fail because the call has multiple
    // observed targets.
    MyClass.prototype.getA = function() {
        return 222;
    }
    p.map(function(e) { return e.getA(); },
          {mode: "par", expect: "disqualified"});
}

testMap();

