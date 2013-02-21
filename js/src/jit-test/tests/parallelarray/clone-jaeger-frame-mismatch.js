// Shouldn't crash.

var baz = new Array(100);
function F() { }
baz.__proto__ = new F(); // <-- need this to expose issue
 
for (var i=0; i < 99; i++) {
  // ^-- this threshold needs to be high to expose issue
  new ParallelArray([1,1], function (i,j) baz[i], undefined);
}
