// -*- mode: js2; indent-tabs-mode: nil; -*-

// A RectArray<X,k> is a subclass of Array with 'width', 'height',
// and 'payload' properties (where payload holds 'k').
// properties, and 'width*height*k' elements (each an X) in the array.
//
// A RectTypedByteArray<k> is a subclass of ArrayBuffer with 'width'
// and 'height' properties, and 'width*height*k' bytes in the buffer.
//
// The 'payload' property is initialized with the value of 'k'.
//
// (Felix would have used Array or ArrayView, but for the following bug: 
//   Bug 695438 - TypedArrays don't support new named properties
//   https://bugzilla.mozilla.org/show_bug.cgi?id=695438
//  and so he is resorting to extending ArrayBuffer instead.)
//
// Both classes add a .get(x,y[,k]) method that eases access to the
// contents, and a set(x,y[,k],value) method that eases modifying
// their entries.
//
// In addition, for those who prefer functional-style,
// RectArray.build,
// and RectByteTypedArray.build

var RectArray, RectByteTypedArray;

(function (){

   function defineReadOnly(x, name, value) {
     Object.defineProperty(x, name, {
                             value: value,
                             writable: false,
                             enumerable: true,
                             configurable: true });
   }

   RectArray = function RectArray(w,h,k) {
     if (k === undefined)
       k = 1;
     this.length = w*h*k;
     defineReadOnly(this, "width", w);
     defineReadOnly(this, "height", h);
     defineReadOnly(this, "payload", k);
   };

   RectByteTypedArray = function RectByteTypedArray(w,h,k) {
     if (k === undefined)
       k = 1;
     ArrayBuffer.call(this, w*h*k);
     defineReadOnly(this, "width", w);
     defineReadOnly(this, "height", h);
     defineReadOnly(this, "payload", k);
   };

   RectArray.prototype = new Array();
   RectByteTypedArray.prototype = new ArrayBuffer();

   RectArray.prototype.get = function get(x,y,j) {
     if (j === undefined) j = 0;
     return this[(y*this.width+x)*this.payload+j];
   };

   RectArray.prototype.set = function set(x,y,j,value) {
     if (value === undefined) {
       value = j;
       j = 0;
     }
     this[(y*this.width+x)*this.payload+j] = value;
   };

   RectByteTypedArray.prototype.get = function get(x,y,j) {
     if (j === undefined) j = 0;
     return (new Uint8Array(this))[(y*this.width+x)*this.payload+j];
   };

   RectByteTypedArray.prototype.set = function set(x,y,j,value) {
     if (value === undefined) {
       value = j;
       j = 0;
     }
     (new Uint8Array(this))[(y*this.width+x)*this.payload+j] = value;
   };

   function viewToSource(view, width, height, payload) {
     var ret = "[";
     var i=0;
     var matrixNeedsNewline = false;
     for (var row=0; row < height; row++) {
       if (matrixNeedsNewline)
         ret += ",\n ";
       ret += "[";
       var rowNeedsComma = false;
       for (var x=0; x < width; x++) {
         if (rowNeedsComma)
           ret += ", ";
         if (payload == 1) {
           if (view[i] !== undefined)
             ret += view[i];
           i++;
         } else {
           var entryNeedsComma = false;
           ret += "(";
           for (var k=0; k < payload; k++) {
             // Might be inefficient (does JavaScript have
             // StringBuffers?, or use them internally, like Tamarin?)
             if (entryNeedsComma)
               ret += ", ";
             if (view[i] !== undefined)
               ret += view[i];
             entryNeedsComma = true;
             i++;
           }
           ret += ")";
         }
         rowNeedsComma = true;
       }
       ret += "]";
       matrixNeedsNewline = true;
     }
     ret += "]";
     return ret;
   }

   RectArray.prototype.toSource = function toSource() {
     return viewToSource(this,
                         this.width, this.height, this.payload);
   };

   RectByteTypedArray.prototype.toSource = function toSource() {
     return viewToSource(new Uint8Array(this),
                         this.width, this.height, this.payload);
   };

   // (Array<X>|ArrayView<X>) Nat Nat Nat (Nat Nat Nat -> X) -> void
   function fillArrayView(view, width, height, k, fill) {
     var i = 0;
     for (var y=0; y < height; y++) {
       for (var x=0; x < width; x++) {
         for (var j=0; j < k; j++) {
           view[i++] = fill(x, y, k);
         }
       }
     }
   }


   // Nat Nat (Nat Nat [Nat] -> X) -> RectArray<X,1>
   RectArray.build =
     function buildRectArray1(width, height, fill) {
       var a = new RectArray(width, height, 1);
       fillArrayView(a, width, height, 1, fill);
       return a;
     };

   // Nat Nat (Nat Nat Nat -> X) -> RectArray<X,4>
   RectArray.build4 =
     function buildRectArray4(width, height, fill) {
       var a = new RectArray(width, height, 4);
       fillArrayView(a, width, height, 4, fill);
       return a;
     };

   // Nat Nat (Nat Nat Nat -> X) -> RectArray<X,1>
   RectArray.buildN =
     function buildRectArray4(width, height, n, fill) {
       var a = new RectArray(width, height, n);
       fillArrayView(a, width, height, n, fill);
       return a;
     };


   // Nat Nat (Nat Nat [Nat] -> Byte) -> RectTypedByteArray<4>
   RectByteTypedArray.build =
     function buildRectByteTypedArray1(width, height, fill) {
       var buf = new RectByteTypedArray(width, height, 1);
       fillArrayView(new Uint8Array(buf), width, height, 1, fill);
       return buf;
     };

   // Nat Nat (Nat Nat Nat -> Byte) -> RectTypedByteArray<4>
   RectByteTypedArray.build4 =
     function buildRectByteTypedArray4(width, height, fill) {
       var buf = new RectByteTypedArray(width, height, 4);
       fillArrayView(new Uint8Array(buf), width, height, 4, fill);
       return buf;
     };

   // Nat Nat (Nat Nat Nat -> Byte) -> RectTypedByteArray<4>
   RectByteTypedArray.buildN =
     function buildRectByteTypedArray4(width, height, n, fill) {
       var buf = new RectByteTypedArray(width, height, n);
       fillArrayView(new Uint8Array(buf), width, height, n, fill);
       return buf;
     };

 })();
