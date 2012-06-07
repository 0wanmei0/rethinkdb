import subprocess, os, time, string, signal
from vcoptparse import *

def run(command_line, host, port, timeout):
    start_time = time.time()
    end_time = start_time + timeout
    print "Running %r..." % command_line

    # Set up environment
    new_environ = os.environ.copy()
    new_environ["HOST"] = host
    new_environ["PORT"] = str(port)

    proc = subprocess.Popen(command_line, shell = True, env = new_environ, preexec_fn = lambda: os.setpgid(0, 0))

    try:
        while time.time() < end_time:
            result = proc.poll()
            if result is None:
                time.sleep(1)
            elif result == 0:
                print "Done (%d seconds)" % (time.time() - start_time)
                return
            else:
                print "Failed (%d seconds)" % (time.time() - start_time)
                raise RuntimeError("workload '%s' failed with error code %d" % (command_line, result))
        print "Timed out (%d seconds)" % (time.time() - start_time)
    finally:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except OSError:
            pass
    raise RuntimeError("workload timed out before completion")

class ContinuousWorkload(object):
    def __init__(self, command_line, host, port):
        self.command_line = command_line
        self.host = host
        self.port = port
        self.running = False

    def __enter__(self):
        return self

    def start(self):
        assert not self.running
        print "Starting %r..." % self.command_line

        # Set up environment
        new_environ = os.environ.copy()
        new_environ["HOST"] = self.host
        new_environ["PORT"] = str(self.port)

        self.proc = subprocess.Popen(self.command_line, shell = True, env = new_environ, preexec_fn = lambda: os.setpgid(0, 0))

        self.running = True

        self.check()

    def check(self):
        assert self.running
        result = self.proc.poll()
        if result is not None:
            self.running = False
            raise RuntimeError("workload '%s' stopped prematurely with error code %d" % (self.command_line, result))

    def stop(self):
        self.check()
        print "Stopping %r..." % self.command_line
        os.killpg(self.proc.pid, signal.SIGINT)
        shutdown_grace_period = 10   # seconds
        end_time = time.time() + shutdown_grace_period
        while time.time() < end_time:
            result = self.proc.poll()
            if result is None:
                time.sleep(1)
            elif result == 0 or result == -signal.SIGINT:
                print "OK"
                self.running = False
                break
            else:
                self.running = False
                raise RuntimeError("workload '%s' failed when interrupted with error code %d" % (self.command_line, result))
        else:
            raise RuntimeError("workload '%s' failed to terminate within %d seconds of SIGINT" % (self.command_line, shutdown_grace_period))

    def __exit__(self, exc = None, ty = None, tb = None):
        if self.running:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except OSError:
                pass

# A lot of scenarios work either with a two-phase split workload or a continuous
# workload. This code factors out the details of parsing and handling that. The
# syntax for invoking such scenarios is as follows:
#     scenario.py --split-workload 'workload1.py $HOST:$PORT' 'workload2.py $HOST:$PORT' [--timeout timeout]
#     scenario.py --continuous-workload 'workload.py $HOST:$PORT'

def prepare_option_parser_for_split_or_continuous_workload(op):
    op["workload-before"] = StringFlag("--workload-before", None)
    op["timeout-before"] = IntFlag("--timeout-before", 600)
    op["workload-during"] = StringFlag("--workload-during", None)
    op["extra-before"] = IntFlag("--extra-before", 10)
    op["extra-after"] = IntFlag("--extra-after", 10)
    op["workload-after"] = StringFlag("--workload-after", None)
    op["timeout-after"] = IntFlag("--timeout-after", 600)

class SplitOrContinuousWorkload(object):
    def __init__(self, opts, host, port):
        self.opts, self.host, self.port = opts, host, port
    def __enter__(self):
        if self.opts["workload-during"] is not None:
            self.continuous_workload = ContinuousWorkload(self.opts["workload-during"], self.host, self.port)
            self.continuous_workload.__enter__()
        return self
    def step1(self):
        if self.opts["workload-before"] is not None:
            run(self.opts["workload-before"], self.host, self.port, self.opts["timeout-before"])
        if self.opts["workload-during"] is not None:
            self.continuous_workload.start()
            if self.opts["extra-before"] != 0:
                print "Letting %r run for %d seconds..." % (self.opts["workload-during"], self.opts["extra-before"])
                time.sleep(self.opts["extra-before"])
                self.continuous_workload.check()
    def check(self):
        if self.opts["workload-during"] is not None:
            self.continuous_workload.check()
    def step2(self):
        if self.opts["workload-during"] is not None:
            if self.opts["extra-after"] != 0:
                self.continuous_workload.check()
                print "Letting %r run for %d seconds..." % (self.opts["workload-during"], self.opts["extra-after"])
                time.sleep(self.opts["extra-after"])
            self.continuous_workload.stop()
        if self.opts["workload-after"] is not None:
            run(self.opts["workload-after"], self.host, self.port, self.opts["timeout-after"])
    def __exit__(self, exc = None, ty = None, tb = None):
        if self.opts["workload-during"] is not None:
            self.continuous_workload.__exit__()
