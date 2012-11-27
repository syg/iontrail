function measure(f) {
  var start = new Date();
  result = f();
  var end = new Date();
  print("Time required: ", end.getTime() - start.getTime());
  return result;
}

function testMap() {
  function seq() {
    var r = [];
    for (var i = 0; i < array.length; i++)
      r[i] = inc(array[i]);
    return r;
  }

  function inc(v) {
    return v+1;
  }

  var array = [];
  for (var i = 1; i <= 10*1024*1024; i++) array.push(i % 256);

  print("## testMap");
  print("# warmup");
  print("array length", array.length);
  //seq();
  //seq();
  //seq();
  print("# sequential");
  var expected = measure(seq);

  for (var j = 0; j < 4; j++) {
    print("# run", j);
    var parray = new ParallelArray(array);
    var actual = measure(function() { return parray.map(inc); });
  }
}

testMap();
