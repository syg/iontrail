function measure(f) {
  var start = new Date();
  result = f();
  var end = new Date();
  print("Time required: ", end.getTime() - start.getTime());
  return result;
}

function testMap() {
  function inc(v) {
    return v+1;
  }

  var array = [];
  for (var i = 1; i <= 1024*1024; i++) array.push(i % 256);

  print("## testMap");
  print("# sequential");
  print("array length", array.length);
  var expected = measure(function() {
    var r = [];
    for (var i = 0; i < array.length; i++)
      r[i] = inc(array[i]);
    return r;
  });

  for (var j = 0; j < 4; j++) {
    print("# run", j);
    var parray = new ParallelArray(array);
    var actual = measure(function() { return parray.map(inc); });
  }
}

testMap();
