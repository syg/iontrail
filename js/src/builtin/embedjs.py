# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#
# ToCAsciiArray and ToCArray are from V8's js2c.py.
#
# Copyright 2012 the V8 project authors. All rights reserved.
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
#       copyright notice, this list of conditions and the following
#       disclaimer in the documentation and/or other materials provided
#       with the distribution.
#     * Neither the name of Google Inc. nor the names of its
#       contributors may be used to endorse or promote products derived
#       from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# This utility converts JS files containing self-hosted builtins into a C
# header file that can be embedded into SpiderMonkey.
#
# It expects error messages in the JS code to be referenced by their C enum
# keys as literals.

from __future__ import with_statement
import re, sys, os, fileinput
import StringIO
from optparse import OptionParser
import Preprocessor

def ToCAsciiArray(lines):
  result = []
  for chr in lines:
    value = ord(chr)
    assert value < 128
    result.append(str(value))
  return ", ".join(result)

def ToCArray(lines):
  result = []
  for chr in lines:
    result.append(str(ord(chr)))
  return ", ".join(result)

HEADER_TEMPLATE = """\
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

namespace js {
namespace selfhosted {
  static const char data[] = { %(sources_data)s };

%(sources_declaration)s

  uint32_t GetCompressedSize() {
    return %(compressed_total_length)i;
  }

  uint32_t GetRawScriptsSize() {
    return %(raw_total_length)i;
  }
} // selfhosted
} // js
"""

RAW_SOURCES_DECLARATION = """\
  static const char *rawSources = reinterpret_cast<const char *>(data);
"""

COMPRESSED_SOURCES_DECLARATION = """\
  static const unsigned char *compressedSources = reinterpret_cast<const unsigned char *>(data);
"""

def embed(sources, c_out, js_out, env):
  pp = Preprocessor.Preprocessor()
  pp.out = StringIO.StringIO()
  pp.setMarker('#')
  for k in env:
    pp.context[k] = env[k]
  pp.do_filter('slashstar slashslash macros')
  for source in sources:
    pp.do_include(source)
  processed = pp.out.getvalue()
  with open(js_out, 'w') as output:
    output.write(processed)
  with open(c_out, 'w') as output:
    if 'DEBUG' in env or not ('USE_ZLIB' in env):
      data = ToCAsciiArray(processed)
      output.write(HEADER_TEMPLATE % {
          'sources_data': data,
          'sources_declaration': RAW_SOURCES_DECLARATION,
          'compressed_total_length': 0,
          'raw_total_length': len(processed)
      })
    else:
      import zlib
      compressed = zlib.compress(processed)
      data = ToCArray(compressed)
      output.write(HEADER_TEMPLATE % {
          'sources_data': data,
          'sources_declaration': COMPRESSED_SOURCES_DECLARATION,
          'compressed_total_length': len(compressed),
          'raw_total_length': len(processed)
      })

  pp.out.close()

def main():
  env = {}
  def define_env(option, opt, value, parser):
    pair = value.split('=', 1)
    if len(pair) == 1:
      pair.append(1)
    env[pair[0]] = pair[1]
  p = OptionParser(usage="%prog [options] file")
  p.add_option('-D', action='callback', callback=define_env, type="string",
               metavar='var=[val]', help='Define a variable')
  p.add_option('-o', type='string', metavar='filename', default='selfhosted.out.h',
               help='C array header file')
  p.add_option('-s', type='string', metavar='filename', default='selfhosted.js',
               help='Combined postprocessed JS file')
  (options, sources) = p.parse_args()
  if not (options.o and options.s and sources):
    p.print_help()
    exit(1)
  embed(sources, options.o, options.s, env)

if __name__ == "__main__":
  main()
