// This file expects the lib/parallelarray-helpers.js file
// to already be loaded when you run it.
// I.e. when using js shell, run in this fashion:
//
// js -f path/to/parallelarray-helpers.js -f path/to/this-file.js

var outer_scale = 20;
var inner_iters = 6;
var len = 1000;

function anUnevenComputation(x) {
  var ret = [x+4];
  var outer_iters = 2;
  if ((x != 1 && x < 5) || x > (len - 5))
      outer_iters = (x+1)*outer_scale;

  for (var i = 0; i < outer_iters; i++) {
    ret = [3];
    for (var j = 0; j < inner_iters; j++) {
      ret = ret.concat(ret);
    }
  }
  return ret;
}

var go =
  (function () {
     var i = 0;
     return function() {
       i++;
       print("doing uneven-alloc "+i);
       var mandelbrot = new ParallelArray(len, anUnevenComputation);
     };})();

function repeat(i,body) { while (i > 0) { body(); i--; } }

repeat(10, go);
