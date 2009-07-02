#!/usr/bin/python

# (C) 2009 Clark Williams <williams@redhat.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License Version 2
# as published by the Free Software Foundation.

import sys
import os
import time
import subprocess
import errno

version = "0.5"
debugging = False
quiet = False

def debug(str):
    if debugging: print(str)

def info(str):
    if not quiet: print(str)

#
# Class used to manage mounting and umounting the debugfs
# filesystem. Note that if an instance of this class mounts
# the debufs, it will unmount when cleaning up, but if it 
# discovers that debugfs is already mounted, it will leave
# it mounted.
#
class DebugFS(object):
    '''class to manage mounting/umounting the debugfs'''
    def __init__(self):
        self.premounted = False
        self.mounted = False
        self.mountpoint = ''
        f = open('/proc/mounts')
        for l in f:
            field = l.split()
            if field[2] == "debugfs":
                self.premounted = True
                self.mountpoint = field[1]
                break
        f.close()

    def mount(self, path='/sys/kernel/debug'):
        if self.premounted or self.mounted:
            debug("not mounting debugfs")
            return True
        debug("mounting debugfs at %s" % path)
        self.mountpoint = path
        cmd = ['/bin/mount', '-t', 'debugfs', 'none', path]
        self.mounted = (subprocess.call(cmd) == 0)
        if not self.mounted:
            raise RuntimeError, "Failed to mount debugfs"
        return self.mounted

    def umount(self):
        if self.premounted or not self.mounted:
            debug("not umounting debugfs")
            return True
        debug("umounting debugfs")
        cmd = ['/bin/umount', self.mountpoint]
        self.mounted = not (subprocess.call(cmd) == 0)
        if self.mounted:
            raise RuntimeError, "Failed to umount debugfs"
        return not self.mounted

    def getval(self, item, nonblocking=False):
        path = os.path.join(self.mountpoint, item)
        if nonblocking == False:
            f = open(path)
            val = f.readline()
            f.close()
        else:
            fd = os.open(path, os.O_RDONLY|os.O_NONBLOCK)
            try:
                val = os.read(fd, 256)
            except OSError, e:
                if e.errno == errno.EAGAIN:
                    val = None
                else:
                    raise
            os.close(fd)
        return val

    def putval(self, item, value):
        path = os.path.join(self.mountpoint, item)
        f = open(path, "w")
        f.write(str(value))
        f.flush()
        f.close()

    def getpath(self, item):
        return os.path.join(self.mountpoint, item)

#
# Class used to manage loading and unloading of the 
# hwlat kernel module. Like the debugfs class 
# above, if the module is already loaded, this class will
# leave it alone when cleaning up.
#
class Kmod(object):
    ''' class to manage loading and unloading hwlat.ko'''
    def __init__(self, module='hwlat_detector'):
        self.modname = module
        self.preloaded = False
        f = open ('/proc/modules')
        for l in f:
            field = l.split()
            if field[0] == self.modname:
                self.preloaded = True
                break
        f.close

    def load(self):
        if self.preloaded:
            debug("not loading %s (already loaded)" % self.modname)
            return True
        cmd = ['/sbin/modprobe', self.modname]
        return (subprocess.call(cmd) == 0)

    def unload(self):
        if self.preloaded:
            debug("Not unloading %s" % self.modname)
            return True
        cmd = ['/sbin/modprobe', '-r', self.modname]
        return (subprocess.call(cmd) == 0)

#
# Class to simplify running the hwlat kernel module
#
class Hwlat(object):
    '''class to wrap access to hwlat debugfs files'''
    def __init__(self):
        if os.getuid() != 0:
            raise RuntimeError, "Must be root"
        self.debugfs = DebugFS()
        self.kmod = Kmod()
        self.setup()
        self.testduration = 10 # ten seconds
        self.samples = []

    def force_cleanup(self):
        debug("forcing unload of hwlat module")
        self.kmod.preloaded = False
        debug("forcing unmount of debugfs")
        self.debugfs.premounted = False
        self.cleanup()
        debug("exiting")
        sys.exit(0)

    def setup(self):
        if not self.debugfs.mount():
            raise RuntimeError, "Failed to mount debugfs"
        if not self.kmod.load():
            raise RuntimeError, "Failed to unload hwlat"

    def cleanup(self):
        if not self.kmod.unload():
            raise RuntimeError, "Failed to unload hwlat"
        if not self.debugfs.umount():
            raise RuntimeError, "Failed to unmount debugfs"

    def get(self, field):
        return int(self.debugfs.getval(os.path.join("hwlat_detector", field)))

    def set(self, field, val):
        if field == "enable" and val:
            val = 1
        self.debugfs.putval(os.path.join("hwlat_detector", field), str(val))

    def get_sample(self):
        return self.debugfs.getval("hwlat_detector/sample", nonblocking=True)

    def start(self):
        debug("enabling hwlat module")
        count = 0
        self.set("enable", 1)
        while self.get("enable") == 0:
            count += 1
            debug("retrying setting enable to 1 (%d)" % count)
            time.sleep(0.1)
            self.set("enable", 1)

    def stop(self):
        debug("disabling hwlat module")
        count = 0
        self.set("enable", 0)
        while self.get("enable") == 1:
            count += 1
            debug("retrying setting enable to zero(%d)" % count)
            time.sleep(0.1)
            self.set("enable", 0)

    def detect(self):
        self.samples = []
        testend = time.time() + self.testduration
        debug("Starting hardware latency detection for %d seconds" % (self.testduration))
        try:
            pollcnt = 0
            self.start()
            try:
                while time.time() < testend:
                    pollcnt += 1
                    val = self.get_sample()
                    while val:
                        self.samples.append(val.strip())
                        debug("got a latency sample: %s" % val.strip())
                        val = self.get_sample()
                    time.sleep(0.1)
            except KeyboardInterrupt, e:
                print "interrupted"
                sys.exit(1)
        finally:
            debug("Stopping hardware latency detection (poll count: %d" % pollcnt)
            self.stop()
        debug("Hardware latency detection done (%d samples)" % len(self.samples))

def seconds(str):
    "convert input string to value in seconds"
    if str.isdigit():
        return int(str)
    elif str[-2].isalpha():
        raise RuntimeError, "illegal suffix for seconds: '%s'" % str[-2:-1]
    elif str[-1:] == 's':
        return int(str[0:-1])
    elif str[-1:] == 'm':
        return int(str[0:-1]) * 60
    elif str[-1:] == 'h':
        return int(str[0:-1]) * 3600
    elif str[-1:] == 'd':
        return int(str[0:-1]) * 86400
    elif str[-1:] == 'w':
        return int(str[0:-1]) * 86400 * 7
    else:
        raise RuntimeError, "unknown suffix for second conversion: '%s'" % str[-1]


def microseconds(str):
    "convert input string to microsecond value"
    if str[-2:] == 'ms':
        return (int(str[0:-2]) * 1000)
    elif str[-2:] == 'us':
        return int(str[0:-2])
    elif str[-1:] == 's':
        return (int(str[0:-1]) * 1000 * 1000)
    elif str[-1:].isalpha():
        raise RuntimeError, "unknown suffix for microsecond conversion: '%s'" % str[-1]
    else:
        return int(str)

if __name__ == '__main__':
    from optparse import OptionParser

    parser = OptionParser()
    parser.add_option("--duration", default=None, type="string",
                      dest="duration",
                      help="total time to test for hardware latency (<n>{smdw})")

    parser.add_option("--threshold", default=None, type="string",
                      dest="threshold",
                      help="value above which is considered an hardware latency")

    parser.add_option("--window", default=None, type="string",
                      dest="window",
                      help="time between samples")

    parser.add_option("--width", default=None, type="string",
                      dest="width",
                      help="time to actually measure")

    parser.add_option("--report", default=None, type="string",
                      dest="report",
                      help="filename for sample data")

    parser.add_option("--cleanup", action="store_true", default=False,
                      dest="cleanup",
                      help="force unload of module and umount of debugfs")

    parser.add_option("--debug", action="store_true", default=False,
                      dest="debug",
                      help="turn on debugging prints")

    parser.add_option("--quiet", action="store_true", default=False,
                      dest="quiet",
                      help="turn off all screen output")

    (o, a) = parser.parse_args()

    hwlat = Hwlat()

    if o.debug: 
        debugging = True
        quiet = False
        debug("debugging prints turned on")

    if o.quiet:
        quiet = True
        debugging = False

    if o.cleanup:
        debug("forcing cleanup of debugfs and hardware latency module")
        hwlat.force_cleanup()
        sys.exit(0)

    if o.threshold:
        t = microseconds(o.threshold)
        hwlat.set("threshold", t)
        debug("threshold set to %dus" % t)

    if o.window:
        w = microseconds(o.window)
        if w < hwlat.get("width"):
            debug("shrinking width to %d for new window of %d" % (w/2, w))
            hwlat.set("width", w/2)
        debug("window parameter = %d" % w)
        hwlat.set("window", w)
        debug("window for sampling set to %dus" % w)

    if o.width:
        w = microseconds(o.width)
        if w > hwlat.get("window"):
            debug("widening window to %d for new width of %d" % (w*2, w))
            hwlat.set("window", w*2)
        debug("width parameter = %d" % w)
        hwlat.set("width", w)
        debug("sample width set to %dus" % w)

    if o.duration:
        hwlat.testduration = seconds(o.duration)
    else:
        hwlat.testduration = 120 # 2 minutes
    debug("test duration is %ds" % hwlat.testduration)

    reportfile = o.report

    info("hwlatdetect:  test duration %d seconds" % hwlat.testduration)
    info("   parameters:")
    info("        Latency threshold: %dus" % hwlat.get("threshold"))
    info("        Sample window:     %dus" % hwlat.get("window"))
    info("        Sample width:      %dus" % hwlat.get("width"))
    info("     Non-sampling period:  %dus" % (hwlat.get("window") - hwlat.get("width")))
    info("        Output File:       %s" % reportfile)
    info("\nStarting test")

    hwlat.detect()

    info("test finished")

    exceeding = hwlat.get("count")
    info("Max Latency: %dus" % hwlat.get("max"))
    info("Samples recorded: %d" % len(hwlat.samples))
    info("Samples exceeding threshold: %d" % exceeding)

    if reportfile:
        f = open(reportfile, "w")
        for s in hwlat.samples:
            f.write("%d\n" % s)
        f.close()
        info("sample data written to %s" % reportfile)
    else:
        for s in hwlat.samples:
            print "%s" % s

    hwlat.cleanup()
    sys.exit(exceeding)
