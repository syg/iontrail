/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */
/**
    File Name:          15.1.2.1-1.js
    ECMA Section:       15.1.2.1 eval(x)

                        if x is not a string object, return x.
    Description:
    Author:             christine@netscape.com
    Date:               16 september 1997
*/
    var SECTION = "15.1.2.1-1";
    var VERSION = "ECMA_1";
    startTest();
    var TITLE   = "eval(x)";

    var BUGNUMBER = "111199";

    writeHeaderToLog( SECTION + " "+ TITLE);

    var testcases = getTestCases();

    test();

function getTestCases() {
    var array = new Array();
    var item = 0;

    array[item++] = new TestCase( SECTION,      "eval.length",              1,              eval.length );
    array[item++] = new TestCase( SECTION,      "delete eval.length",       false,          delete eval.length );
    array[item++] = new TestCase( SECTION,      "var PROPS = ''; for ( p in eval ) { PROPS += p }; PROPS",  "", eval("var PROPS = ''; for ( p in eval ) { PROPS += p }; PROPS") );
    array[item++] = new TestCase( SECTION,      "eval.length = null; eval.length",       1, eval( "eval.length = null; eval.length") );
//    array[item++] = new TestCase( SECTION,     "eval.__proto__",                       Function.prototype,            eval.__proto__ );

    // test cases where argument is not a string.  should return the argument.

    array[item++] = new TestCase( SECTION,     "eval()",                                void 0,                     eval() );
    array[item++] = new TestCase( SECTION,     "eval(void 0)",                          void 0,                     eval( void 0) );
    array[item++] = new TestCase( SECTION,     "eval(null)",                            null,                       eval( null ) );
    array[item++] = new TestCase( SECTION,     "eval(true)",                            true,                       eval( true ) );
    array[item++] = new TestCase( SECTION,     "eval(false)",                           false,                      eval( false ) );

    array[item++] = new TestCase( SECTION,     "typeof eval(new String('Infinity/-0')", "object",                   typeof eval(new String('Infinity/-0')) );

    array[item++] = new TestCase( SECTION,     "eval([1,2,3,4,5,6])",                  "1,2,3,4,5,6",                 ""+eval([1,2,3,4,5,6]) );
    array[item++] = new TestCase( SECTION,     "eval(new Array(0,1,2,3)",              "1,2,3",                       ""+  eval(new Array(1,2,3)) );
    array[item++] = new TestCase( SECTION,     "eval(1)",                              1,                             eval(1) );
    array[item++] = new TestCase( SECTION,     "eval(0)",                              0,                             eval(0) );
    array[item++] = new TestCase( SECTION,     "eval(-1)",                             -1,                            eval(-1) );
    array[item++] = new TestCase( SECTION,     "eval(Number.NaN)",                     Number.NaN,                    eval(Number.NaN) );
    array[item++] = new TestCase( SECTION,     "eval(Number.MIN_VALUE)",               5e-308,                        eval(Number.MIN_VALUE) );
    array[item++] = new TestCase( SECTION,     "eval(-Number.MIN_VALUE)",              -5e-308,                       eval(-Number.MIN_VALUE) );
    array[item++] = new TestCase( SECTION,     "eval(Number.POSITIVE_INFINITY)",       Number.POSITIVE_INFINITY,      eval(Number.POSITIVE_INFINITY) );
    array[item++] = new TestCase( SECTION,     "eval(Number.NEGATIVE_INFINITY)",       Number.NEGATIVE_INFINITY,      eval(Number.NEGATIVE_INFINITY) );
    array[item++] = new TestCase( SECTION,     "eval( 4294967296 )",                   4294967296,                    eval(4294967296) );
    array[item++] = new TestCase( SECTION,     "eval( 2147483648 )",                   2147483648,                    eval(2147483648) );

    return ( array );
}
function test() {
    for ( tc=0; tc < testcases.length; tc++ ) {
        testcases[tc].passed = writeTestCaseResult(
                            testcases[tc].expect,
                            testcases[tc].actual,
                            testcases[tc].description +" = "+ testcases[tc].actual );

        testcases[tc].reason += ( testcases[tc].passed ) ? "" : "wrong value ";
    }

    stopTest();
    return ( testcases );
}
