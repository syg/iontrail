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
	Filename:     regexp.js
	Description:  'Tests regular expressions using flags "i" and "g"'

	Author:       Nick Lerissa
	Date:         March 10, 1998
*/

	var SECTION = 'As described in Netscape doc "Whats new in JavaScript 1.2"';
	var VERSION = 'no version';
    startTest();
	var TITLE   = 'regular expression flags with flags "i" and "g"';

	writeHeaderToLog('Executing script: flags.js');
	writeHeaderToLog( SECTION + " "+ TITLE);

	var count = 0;
	var testcases = new Array();

    // testing optional flag 'i'
	testcases[count++] = new TestCase ( SECTION, "'aBCdEfGHijKLmno'.match(/fghijk/i)",
	                                    String(["fGHijK"]), String('aBCdEfGHijKLmno'.match(/fghijk/i)));

	testcases[count++] = new TestCase ( SECTION, "'aBCdEfGHijKLmno'.match(new RegExp('fghijk','i'))",
	                                    String(["fGHijK"]), String('aBCdEfGHijKLmno'.match(new RegExp("fghijk","i"))));

    // testing optional flag 'g'
	testcases[count++] = new TestCase ( SECTION, "'xa xb xc xd xe xf'.match(/x./g)",
	                                    String(["xa","xb","xc","xd","xe","xf"]), String('xa xb xc xd xe xf'.match(/x./g)));

	testcases[count++] = new TestCase ( SECTION, "'xa xb xc xd xe xf'.match(new RegExp('x.','g'))",
	                                    String(["xa","xb","xc","xd","xe","xf"]), String('xa xb xc xd xe xf'.match(new RegExp('x.','g'))));

    // testing optional flags 'g' and 'i'
	testcases[count++] = new TestCase ( SECTION, "'xa Xb xc xd Xe xf'.match(/x./gi)",
	                                    String(["xa","Xb","xc","xd","Xe","xf"]), String('xa Xb xc xd Xe xf'.match(/x./gi)));

	testcases[count++] = new TestCase ( SECTION, "'xa Xb xc xd Xe xf'.match(new RegExp('x.','gi'))",
	                                    String(["xa","Xb","xc","xd","Xe","xf"]), String('xa Xb xc xd Xe xf'.match(new RegExp('x.','gi'))));

	testcases[count++] = new TestCase ( SECTION, "'xa Xb xc xd Xe xf'.match(/x./ig)",
	                                    String(["xa","Xb","xc","xd","Xe","xf"]), String('xa Xb xc xd Xe xf'.match(/x./ig)));

	testcases[count++] = new TestCase ( SECTION, "'xa Xb xc xd Xe xf'.match(new RegExp('x.','ig'))",
	                                    String(["xa","Xb","xc","xd","Xe","xf"]), String('xa Xb xc xd Xe xf'.match(new RegExp('x.','ig'))));


	function test()
	{
	   for ( tc=0; tc < testcases.length; tc++ ) {
	        testcases[tc].passed = writeTestCaseResult(
	        testcases[tc].expect,
	        testcases[tc].actual,
	        testcases[tc].description +" = "+
	        testcases[tc].actual );
	        testcases[tc].reason += ( testcases[tc].passed ) ? "" : "wrong value ";
	   }
	   stopTest();
	   return ( testcases );
	}

	test();

