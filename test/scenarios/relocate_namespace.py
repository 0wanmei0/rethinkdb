#!/usr/bin/python
import sys, os, time
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), os.path.pardir, 'common')))
import http_admin, driver, workload_runner
from vcoptparse import *

op = OptParser()
op["workload1"] = PositionalArg()
op["workload2"] = PositionalArg()
op["timeout"] = IntFlag("--timeout", 600)
opts = op.parse(sys.argv)

with driver.Metacluster() as metacluster:
    cluster = driver.Cluster(metacluster)
    print "Starting cluster..."
    num_nodes = 2
    processes = [driver.Process(cluster, driver.Files(metacluster, db_path = "db-%d" % i), log_path = "serve-output-%d" % i)
        for i in xrange(num_nodes)]
    time.sleep(3)
    print "Creating namespace..."
    http = http_admin.ClusterAccess([("localhost", p.http_port) for p in processes])
    primary_dc = http.add_datacenter()
    secondary_dc = http.add_datacenter()
    machines = http.machines.keys()
    http.move_server_to_datacenter(machines[0], primary_dc)
    http.move_server_to_datacenter(machines[1], secondary_dc)
    ns = http.add_namespace(protocol = "memcached", primary = primary_dc,
        affinities = {})
    time.sleep(10)
    cluster.check()
    http.check_no_issues()
    host, port = driver.get_namespace_host(ns.port, processes)
    workload_runner.run(opts["workload1"], host, port, opts["timeout"])
    cluster.check()
    http.check_no_issues()
    http.move_namespace_to_datacenter(ns, secondary_dc)
    time.sleep(10)
    cluster.check()
    http.check_no_issues()
    workload_runner.run(opts["workload2"], host, port, opts["timeout"])
    cluster.check_and_close()
