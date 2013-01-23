load(libdir + "parallelarray-helpers.js");

function MyClass(a) {
  this.a = a;
}

MyClass.prototype.getA = function() {
  return this.a;
}

function testMap() {
  var instances = range(22, 22+64).map(function (i) { return new MyClass(i); });

  // first time, everything goes well.
  var p = new ParallelArray(instances);
  var q = p.map(function(e) { return e.getA(); },
                {mode: "par", expect: "success"});
  print(q);
  assertEq(q.get(0), 22);
  assertEq(q.get(1), 23);

  // second time, we mutate getA().  This should trigger a
  // recompilation which will fail because the call has multiple
  // observed targets.
  MyClass.prototype.getA = function() {
    return 222;
  };

  var r = p.map(function(e) { return e.getA(); },
                {mode: "par", expect: "disqualified"});
  print(r);
  assertEq(r.get(0), 222);
  assertEq(r.get(1), 222);
}

testMap();

