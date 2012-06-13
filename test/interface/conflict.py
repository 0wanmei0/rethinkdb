#!/usr/bin/env python
import sys, os, time
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), os.path.pardir, 'common')))
import driver, http_admin
from vcoptparse import *

op = OptParser()
op["mode"] = StringFlag("--mode", "debug")
opts = op.parse(sys.argv)

with driver.Metacluster() as metacluster:
    cluster1 = driver.Cluster(metacluster)
    executable_path = driver.find_rethinkdb_executable(opts["mode"])
    print "Spinning up two processes..."
    files1 = driver.Files(metacluster, executable_path = executable_path)
    proc1 = driver.Process(cluster1, files1, executable_path = executable_path)
    files2 = driver.Files(metacluster, executable_path = executable_path)
    proc2 = driver.Process(cluster1, files2, executable_path = executable_path)
    proc1.wait_until_started_up()
    proc2.wait_until_started_up()
    cluster1.check()

    access1 = http_admin.ClusterAccess([("localhost", proc1.http_port)])
    access2 = http_admin.ClusterAccess([("localhost", proc2.http_port)])
    dc = access1.add_datacenter("Fizz")
    time.sleep(2)
    access2.update_cluster_data()
    assert len(access1.get_directory()) == len(access2.get_directory()) == 2

    print "Splitting cluster, then waiting 20s..."
    cluster2 = driver.Cluster(metacluster)
    metacluster.move_processes(cluster1, cluster2, [proc2])
    time.sleep(20)

    print "Conflicting datacenter name..."
    access1.rename(dc, "Buzz")
    access2.rename(access2.find_datacenter(dc.uuid), "Fizz")

    print "Joining cluster, then waiting 10s..."
    metacluster.move_processes(cluster2, cluster1, [proc2])
    time.sleep(10)
    cluster1.check()
    cluster2.check()
    issues = access1.get_issues()
    assert issues[0]["type"] == "VCLOCK_CONFLICT"
    assert len(access1.get_directory()) == len(access2.get_directory()) == 2
    cluster1.check_and_stop()
print "Done."

