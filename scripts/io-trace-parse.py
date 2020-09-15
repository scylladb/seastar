#!/bin/env python3
#
# Script to parse IO trace logs and show some stats
#

import sys
import statistics


# prints average, .99 quantile and maximum value for an array
def print_stat_line(what, st):
    def q99(arr):
        return statistics.quantiles(arr, n=100)[-1]

    print("\t{:18}: avg:{:12.6f}  .99:{:12.6f}  max:{:12.6f}".format(what,
          statistics.fmean(st), q99(st), max(st)))


# Inc/Dec counter that also collects its value history
class counter:
    def __init__(self):
        self._v = 0
        self._stat = []

    def inc(self):
        self._v += 1
        self._stat.append(self._v)

    def dec(self):
        self._v -= 1
        self._stat.append(self._v)

    def stat(self):
        return self._stat


class req:
    def __init__(self, rqlen):
        self.len = rqlen
        self.queue = None
        self.submit = None
        self.complete = None


# Timings for requests
class req_stat:
    def __init__(self):
        self.qtimes = []        # time in queue
        self.xtimes = []        # time in disk
        self.latencies = []     # sum of the above
        self.delays = []        # time between submits
        self.prev = None        # helper for the above
        self.in_queue = counter()
        self.in_disk = counter()

    def queue(self, rq):
        self.in_queue.inc()

    def submit(self, rq):
        if self.prev:
            self.delays.append(rq.submit - self.prev)
        self.prev = rq.submit
        self.qtimes.append(rq.submit - rq.queue)
        self.in_queue.dec()
        self.in_disk.inc()

    def complete(self, rq):
        self.xtimes.append(rq.complete - rq.submit)
        self.latencies.append(rq.complete - rq.queue)
        self.in_disk.dec()

    def show(self, rqlen):
        print("{}k requests".format(int(rqlen/1024)))
        print("\ttotal: {}".format(len(self.latencies)))
        print_stat_line('in queue usec', self.qtimes)
        print_stat_line('      `- num ', self.in_queue.stat())
        print_stat_line('in disk usec', self.xtimes)
        print_stat_line('     `- num ', self.in_disk.stat())
        print_stat_line('latency', self.latencies)
        print_stat_line('period', self.delays)


# Stats for a device. Umbrella-object for the above stats
class device_stat:
    def __init__(self):
        self.reqs = {}          # collection of req's
        self.req_stats = {}     # statistics by request size
        self.in_queue = counter()
        self.in_disk = counter()

    def queue(self, rqid, ts, rqlen):
        rq = req(rqlen)
        self.reqs[rqid] = rq
        rq.queue = ts
        if rq.len not in self.req_stats:
            self.req_stats[rq.len] = req_stat()
        st = self.req_stats[rq.len]
        st.queue(rq)
        self.in_queue.inc()

    def submit(self, rqid, ts):
        rq = self.reqs[rqid]
        rq.submit = ts
        st = self.req_stats[rq.len]
        st.submit(rq)
        self.in_queue.dec()
        self.in_disk.inc()

    def complete(self, rqid, ts):
        rq = self.reqs[rqid]
        rq.complete = ts
        st = self.req_stats[rq.len]
        st.complete(rq)
        del self.reqs[rqid]
        self.in_disk.dec()

    def _show_req_stats(self):
        for rlen in self.req_stats:
            st = self.req_stats[rlen]
            st.show(rlen)

    def _show_queue_stats(self):
        print("queue")
        print_stat_line('in queue num:', self.in_queue.stat())
        print_stat_line('in disk num:', self.in_disk.stat())

    def show(self, devid):
        print("{}".format(devid).center(80, "-"))
        self._show_req_stats()
        self._show_queue_stats()


class parser:
    def __init__(self, f):
        self._file = f
        self._dev_stats = {}

    def _get_dev_stats(self, devid):
        if devid not in self._dev_stats:
            self._dev_stats[devid] = device_stat()

        return self._dev_stats[devid]

    def _parse_req_event(self, ln):
        req_id = ln[10]
        ts = float(ln[1])
        st = self._get_dev_stats(int(ln[7]))

        if ln[11] == 'queue':
            st.queue(req_id, ts, int(ln[13]))
        elif ln[11] == 'submit':
            st.submit(req_id, ts)
        elif ln[11] == 'complete':
            st.complete(req_id, ts)

    def _parse_line(self, ln):
        if ln[4] == 'io':
            if ln[9] == 'req':
                self._parse_req_event(ln)

    def parse(self):
        for ln in self._file:
            if ln.startswith('TRACE'):
                self._parse_line(ln.strip().split())

        return self._dev_stats


if __name__ == "__main__":
    p = parser(sys.stdin)
    stats = p.parse()
    for devid in stats:
        stats[devid].show(devid)
