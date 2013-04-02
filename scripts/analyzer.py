#!/usr/bin/python

import sys
import simpletrace

class DataplaneAnalyzer(simpletrace.Analyzer):
    def __init__(self): 
        self.events = {}
        self.cur_flush_start = 0
        self.cur_flush_end = 0
        self.cur_write_total = 0
        self.cur_write_wait_total = 0
        self.cur_write_start = 0
        self.cur_write_end = 0

    def append_event(self, time, dataplane, name, event_data={}):
        event = { 'name': name, 'time': time, 'data': event_data }
        if not self.events.has_key('dataplane'):
            self.events[dataplane] = []
        
        self.events[dataplane].append(event)
    
    def virtio_net_data_plane_start(self, time, dataplane):
        pass

    def virtio_net_data_plane_tx_flush(self, time, dataplane):
        self.cur_flush_start = time

    def virtio_net_data_plane_tx_flush_complete(self, time, dataplane, buffers, bytes_written):
        self.cur_flush_end = time
        flush_duration = self.cur_flush_end - self.cur_flush_start
        #if self.cur_write_wait_total > 0:
        #if float(self.cur_write_total) / flush_duration > .1 or self.cur_write_wait_total > 0:
        if flush_duration > 1000:
            print "flush duration: %d, write duration total: %d, write wait total: %d, buffers: %d, bytes: %d" % (flush_duration, self.cur_write_total, self.cur_write_wait_total, buffers, bytes_written)

        self.cur_write_total = 0
        self.cur_write_wait_total = 0

    def virtio_net_data_plane_tx_write(self, time, dataplane, fd, iovcnt):
        self.cur_write_start = time
        print "writing, iovcnt: %d" % iovcnt

    def virtio_net_data_plane_tx_write_complete(self, time, dataplane, fd, bytes_written):
        self.cur_write_end = time
        if bytes_written < 0:
            self.cur_write_wait_total += self.cur_write_end - self.cur_write_start
        else:
            self.cur_write_total += self.cur_write_end - self.cur_write_start
        print "wrote %d bytes" % bytes_written

    def end(self):
        pass

if len(sys.argv) != 3:
    sys.stderr.write('usage: %s <trace-events> <trace-file>')

trace_events, trace_file = sys.argv[1:]

analyzer = DataplaneAnalyzer()
simpletrace.process(trace_events, trace_file, analyzer)
