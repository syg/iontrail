# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""
Super dumb tokenizer with literal and identifier processing for Preprocessor
"""

import sys

class StupidLexer:
  @staticmethod
  def isidentc(c, start = False):
    return ((c >= 'a' and c <= 'z' ) or
            (c >= 'A' and c <= 'Z') or
            (c == '_') or (c == '$') or
            (not start and c >= '0' and c <= '9'))

  @staticmethod
  def isident(ident):
    return (StupidLexer.isidentc(ident[0], True) and
            all([StupidLexer.isidentc(c) for c in ident[1:]]))

  def __init__(self, line, match_re = True):
    self.line = line
    self.cursor = 0
    self.match_re = match_re

  def getc(self):
    c = self.line[self.cursor]
    self.cursor += 1
    return c

  def peekc(self):
    if self.done():
      return None
    return self.line[self.cursor]

  def reset(line):
    self.line = line
    self.cursor = 0

  def done(self):
    return len(self.line) == self.cursor

  def get(self):
    if self.done():
      return None

    c = self.getc()
    token = c

    # Process whitespace
    if c.isspace():
      while not self.done() and self.peekc().isspace():
        token += self.getc()

    # Process string literals. Be tolerant; even if string is unclosed, return
    # what we have.
    if c == '\'' or c == '"':
      while not self.done():
        c2 = self.getc()
        token += c2
        # Skip escapes
        if c2 == '\\' and self.peekc() == c:
          token += self.getc()
          continue
        if c2 == c:
          break

    # Process regexp literals with backtracking, probably accurate enough for
    # JS.
    if self.match_re and c == '/':
      save = self.cursor
      found = False
      while not self.done():
        c2 = self.getc()
        token += c2
        if c2 == '\\' and self.peekc() == c:
          token += self.getc()
          continue
        if c2 == c:
          found = True
          break
      if not found:
        self.cursor = save
        return c

    # Process numeric literals.
    if c.isdigit():
      while not self.done() and self.peekc().isdigit():
        token += self.getc()

    # Process identifiers.
    if StupidLexer.isidentc(c, True):
      while not self.done() and StupidLexer.isidentc(self.peekc()):
        token += self.getc()

    return token

  def peek(self):
    save = self.cursor
    token = self.get()
    self.cursor = save
    return token

  def passthrough(self):
    t = self.get()
    if t is None:
      return '\n'
    l = ''
    while t is not None:
      l += t
      t = self.get()
    return l

if __name__ == '__main__':
  for l in sys.stdin:
    lexer = StupidLexer(l)
    tokens = []
    t = lexer.get()
    while t is not None:
      tokens.append(t)
      t = lexer.get()
    print tokens
