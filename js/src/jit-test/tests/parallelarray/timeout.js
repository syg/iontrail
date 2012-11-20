// |jit-test| exitstatus: 6;

load(libdir + "parallelarray-helpers.js");

// this is the actual mandelbrot computation, ported to JavaScript
// from the WebCL / OpenCL example at
// http://www.ibiblio.org/e-notes/webcl/mandelbrot.html
function computeSetByRow(x, y) {
    var Cr = (x - 256) / scale + 0.407476;
    var Ci = (y - 256) / scale + 0.234204;
    var I = 0, R = 0, I2 = 0, R2 = 0;
    var n = 0;
    while ((R2+I2 < 2.0) && (n < 512)) {
        I = (R+R)*I+Ci;
        R = R2-I2+Cr;
        R2 = R*R;
        I2 = I*I;
        n++;
    }
    return n;
}

var scale = 10000*300;
timeout(1);
new ParallelArray([2048, 2048], computeSetByRow,
                  { mode: "par", expect: "fatal" });
