# Copyright (c) 2000-2001 ActiveState Tool Corporation.
# See the file LICENSE.txt for licensing information.

# Test our support for the interfaces defined in nsISupportsPrimitives.idl
#
# The framework supports nsISupportsString and nsISupportsWString, but
# only if our class doesnt provide explicit support.

from xpcom import components

class NoSupportsString:
    _com_interfaces_ = [components.interfaces.nsISupports]
    pass

class ImplicitSupportsString:
    _com_interfaces_ = [components.interfaces.nsISupports]
    def __str__(self):
        return "<MyImplicitStrObject>"

class ExplicitSupportsString:
    _com_interfaces_ = [components.interfaces.nsISupports, components.interfaces.nsISupportsString]
    # __str__ will be ignored by XPCOM, as we have _explicit_ support.
    def __str__(self):
        return "<MyImplicitStrObject>"
    # This is the one that will be used.
    def toString(self):
        return "<MyExplicitStrObject>"

class ImplicitSupportsInt:
    _com_interfaces_ = [components.interfaces.nsISupports]
    def __int__(self):
        return 99

class ExplicitSupportsInt:
    _com_interfaces_ = [components.interfaces.nsISupportsPRInt32]
    def get_data(self):
        return 99

class ImplicitSupportsLong:
    _com_interfaces_ = [components.interfaces.nsISupports]
    def __long__(self):
        return 99L

class ExplicitSupportsLong:
    _com_interfaces_ = [components.interfaces.nsISupportsPRInt64]
    def get_data(self):
        return 99

class ExplicitSupportsFloat:
    _com_interfaces_ = [components.interfaces.nsISupportsDouble]
    def get_data(self):
        return 99.99

class ImplicitSupportsFloat:
    _com_interfaces_ = [components.interfaces.nsISupports]
    def __float__(self):
        return 99.99

def test():
    import xpcom.server, xpcom.client
    ob = xpcom.server.WrapObject( NoSupportsString(), components.interfaces.nsISupports)
    if not str(ob).startswith("<XPCOM interface"):
        raise RuntimeError, "Wrong str() value: %s" % (ob,)

    ob = xpcom.server.WrapObject( ImplicitSupportsString(), components.interfaces.nsISupports)
    if str(ob) != "<MyImplicitStrObject>":
        raise RuntimeError, "Wrong str() value: %s" % (ob,)

    ob = xpcom.server.WrapObject( ExplicitSupportsString(), components.interfaces.nsISupports)
    if str(ob) != "<MyExplicitStrObject>":
        raise RuntimeError, "Wrong str() value: %s" % (ob,)

    # Try our conversions.
    try:
        int(ob)
        raise RuntimeError, "Expected to get a ValueError converting this COM object to an int"
    except ValueError:
        pass
    ob = xpcom.server.WrapObject( ExplicitSupportsInt(), components.interfaces.nsISupports)
    if int(ob) != 99:
        raise RuntimeError, "Bad value: %s" % (int(ob),)
    if float(ob) != 99.0:
        raise RuntimeError, "Bad value: %s" % (float(ob),)

    ob = xpcom.server.WrapObject( ImplicitSupportsInt(), components.interfaces.nsISupports)
    if int(ob) != 99:
        raise RuntimeError, "Bad value: %s" % (int(ob),)
    if float(ob) != 99.0:
        raise RuntimeError, "Bad value: %s" % (float(ob),)

    ob = xpcom.server.WrapObject( ExplicitSupportsLong(), components.interfaces.nsISupports)
    if long(ob) != 99 or not repr(long(ob)).endswith("L"):
        raise RuntimeError, "Bad value: %s" % (repr(long(ob)),)
    if float(ob) != 99.0:
        raise RuntimeError, "Bad value: %s" % (float(ob),)

    ob = xpcom.server.WrapObject( ImplicitSupportsLong(), components.interfaces.nsISupports)
    if long(ob) != 99 or not repr(long(ob)).endswith("L"):
        raise RuntimeError, "Bad value: %s" % (repr(long(ob)),)
    if float(ob) != 99.0:
        raise RuntimeError, "Bad value: %s" % (float(ob),)

    ob = xpcom.server.WrapObject( ExplicitSupportsFloat(), components.interfaces.nsISupports)
    if float(ob) != 99.99:
        raise RuntimeError, "Bad value: %s" % (float(ob),)
    if int(ob) != 99:
        raise RuntimeError, "Bad value: %s" % (int(ob),)

    ob = xpcom.server.WrapObject( ImplicitSupportsFloat(), components.interfaces.nsISupports)
    if float(ob) != 99.99:
        raise RuntimeError, "Bad value: %s" % (float(ob),)
    if int(ob) != 99:
        raise RuntimeError, "Bad value: %s" % (int(ob),)

    print "The nsISupports primitive interface tests appeared to work"
test()