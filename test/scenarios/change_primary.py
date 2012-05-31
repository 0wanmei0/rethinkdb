#!/usr/bin/python
import sys, os, time
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), os.path.pardir, 'common')))
import http_admin, driver, workload_runner
from vcoptparse import *

op = OptParser()
workload_runner.prepare_option_parser_for_split_or_continuous_workload(op)
opts = op.parse(sys.argv)

with driver.Metacluster() as metacluster:
    print "Starting cluster..."
    cluster = driver.Cluster(metacluster)
    process1 = driver.Process(cluster, driver.Files(metacluster, db_path = "db-1"), log_path = "serve-output-1")
    process2 = driver.Process(cluster, driver.Files(metacluster, db_path = "db-2"), log_path = "serve-output-2")
    time.sleep(3)

    print "Creating namespace..."
    http = http_admin.ClusterAccess([("localhost", p.http_port) for p in [process1, process2]])
    dc1 = http.add_datacenter()
    http.move_server_to_datacenter(process1.files.machine_name, dc1)
    dc2 = http.add_datacenter()
    http.move_server_to_datacenter(process2.files.machine_name, dc2)
    ns = http.add_namespace(protocol = "memcached", primary = dc1, affinities = {dc1: 0, dc2: 1})
    time.sleep(10)
    cluster.check()
    http.check_no_issues()

    host, port = http.get_namespace_host(ns)
    with workload_runner.SplitOrContinuousWorkload(opts, host, port) as workload:
        workload.step1()
        cluster.check()
        http.check_no_issues()
        workload.check()
        print "Changing the primary..."
        http.set_namespace_affinities(ns, {dc1: 1, dc2: 1})
        http.move_namespace_to_datacenter(ns, dc2)
        http.set_namespace_affinities(ns, {dc1: 1, dc2: 0})
        cluster.check()
        http.check_no_issues()
        workload.step2()

    http.check_no_issues()
    cluster.check_and_close()
