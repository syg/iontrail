load(libdir + "parallelarray-helpers.js");

function MyClass(a) {
  this.a = a;
}

MyClass.prototype.getA = function() {
  return this.a;
}

function testMap() {
  var instances = range(22, 22+64).map(function (i) { return new MyClass(i); });
  var p = new ParallelArray(instances);

  // ICs should work.
  MyClass.prototype.getA = function() {
    return 222;
  };

  var r = p.map(function(e) { return e.getA(); },
                {mode: "par", expect: "success"});
  print(r);
  assertEq(r.get(0), 222);
  assertEq(r.get(1), 222);
}

testMap();

