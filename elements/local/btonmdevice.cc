#include <click/config.h>
#include "btonmdevice.hh"
#include <click/error.hh>
#include <click/etheraddress.hh>
#include <click/args.hh>
#include <click/router.hh>
#include <click/standard/scheduleinfo.hh>
#include <click/packet_anno.hh>
#include <click/straccum.hh>
#include <stdio.h>
#include <unistd.h>
#include <click/master.hh>

# include <sys/socket.h>
# include <sys/ioctl.h>
# include <net/if.h>
# include <net/if_packet.h>
# include <features.h>
# if __GLIBC__ >= 2 && __GLIBC_MINOR__ >= 1
#  include <netpacket/packet.h>
# else
#  include <linux/if_packet.h>
# endif

#include <sys/mman.h>


CLICK_DECLS

BToNMDevice::BToNMDevice()
    : _task(this), _timer(&_task), _q(0), _pulls(0)
{
    _fd = -1;
    _my_fd = false;
    _ringid = -1;
    _full_nm = 1;
    _nm_fd = -1;
    _my_port = -1;
    _nr_ports = 0;
    _cur = 0;
    _my_pkts = 0;
    _debug = false;
    _test = false;
}

BToNMDevice::~BToNMDevice()
{
}

int
BToNMDevice::configure(Vector<String> &conf, ErrorHandler *errh)
{
    int nmexbuf = 0;
    _burst = 1;
    _ringid = -1;
    if (Args(conf, this, errh)
	.read_mp("DEVNAME", _ifname)
	.read("PORT", _my_port)
	.read("NRPORTS", _nr_ports)
	.read("DEBUG", _debug)
	.read("BURST", _burst)
	.read("RING", _ringid)
	.read("FULL_NM", _full_nm)
	.read("TEST", _test)
	.read("NMEXBUF", nmexbuf)
	.complete() < 0)
	return -1;
    if (!_ifname)
	return errh->error("interface not set");
    if (_burst <= 0)
	return errh->error("bad BURST");
    
    if (nmexbuf > 0)
	NetmapInfo::nr_extra_bufs = nmexbuf;

    NetmapInfo::set_dev_dir(_ifname.c_str(), NetmapInfo::dev_tx);
    return 0;
}

FromNMDevice *
BToNMDevice::find_fromnmdevice() const
{
    Router *r = router();
    for (int ei = 0; ei < r->nelements(); ++ei) {
	FromNMDevice *fd =
	    (FromNMDevice *) r->element(ei)->cast("FromNMDevice");
	if (fd && fd->ifname() == _ifname
	    && fd->dev_ringid() == _ringid
	    && fd->fd() >= 0)
	    return fd;
    }
    return 0;
}

int
BToNMDevice::initialize(ErrorHandler *errh)
{
    _timer.initialize(this);

    FromNMDevice *fd = find_fromnmdevice();
    if (fd && fd->netmap()) {
	_fd = fd->fd();
	_netmap = *fd->netmap();
    } else {
	NetmapInfo::initialize(master()->nthreads(), errh);
	if (_ringid >= 0)
	    _fd = _netmap.open_ring(_ifname, _ringid,
				    true, errh);
	else
	    _fd = _netmap.open(_ifname, true, errh);
	if (_fd >= 0) {
	    _my_fd = true;
	    if (!_full_nm) {
		add_select(_fd, SELECT_READ); // NB NOT writable!
		ScheduleInfo::initialize_task(this, &_task, false, errh);
	    }
	} else
	    return -1;
    }
    
    if (_fd >= 0) {
	_netmap.initialize_rings_tx();

	if (_full_nm) {
	    _nm_fd = NetmapInfo::register_thread_poll(_fd, this, NetmapInfo::dev_tx);
	    ScheduleInfo::initialize_task(this, &_task, true, errh);
	}
    }

    char sring[32];
    snprintf(sring, 32, "%d", _ringid);
    // check for duplicate writers
    void *&used = router()->force_attachment("device_writer_" + _ifname + sring);
    if (used)
	return errh->error("duplicate writer for device %<%s:%d%>",
			   _ifname.c_str(), _ringid);
    used = this;

    
//    ScheduleInfo::join_scheduler(this, &_task, errh);
//    _signal = Notifier::upstream_empty_signal(this, 0, &_task);
    return 0;
}

void
BToNMDevice::cleanup(CleanupStage)
{
    if (_full_nm && _nm_fd >= 0) {
	NetmapInfo::poll_fds[_nm_fd]->running = 0;
    }

    if (_fd >= 0 && _my_fd) {
	_netmap.close(_fd);
	_fd = -1;
    }
}


#define __pbatch_next_pkt(i, pb, mpt, dpt, p)	\
    do {					\
	p = 0;					\
	while(i<pb->npkts) {			\
	    if (_my_port >= 0) {		\
		dport = pb->anno_hptr(i);	\
		assert(dport);			\
		if (*dport == _my_port) {	\
		    p = pb->pptrs[i];		\
		    break;			\
		} else				\
		    i++;			\
	    } else {				\
		p = pb->pptrs[i];		\
		break;				\
	    }					\
	}					\
    } while(0)



int
BToNMDevice::netmap_send_batch(PBatch *pb, int from)
{ 
    int i = from;
    Packet *p = 0;
    uint8_t *dport = 0; // dst port offset is 0.

    __pbatch_next_pkt(i, pb, _my_port, dport, p);

    if (!p)
	return i;
    
    for (unsigned ri = _netmap.ring_begin;
	 ri != _netmap.ring_end && p;
	 ++ri)
    {
	struct netmap_ring *ring = NETMAP_TXRING(_netmap.nifp, ri);

	while (p && ring->avail > 0) {
	    unsigned cur = ring->cur;
	    unsigned buf_idx = ring->slot[cur].buf_idx;
	    if (buf_idx < 2)
		continue;
	    unsigned char *buf = (unsigned char *) NETMAP_BUF(ring, buf_idx);
	    uint32_t p_length = p->length();
	    if (NetmapInfo::is_netmap_buffer(p)
		&& !p->shared() /* A little risk: && p->buffer() == p->data() */
		) {
		ring->slot[cur].buf_idx = NETMAP_BUF_IDX(ring, (char *) p->buffer());
		ring->slot[cur].flags |= NS_BUF_CHANGED;
		NetmapInfo::buffer_destructor(buf, 0);
		p->reset_buffer();
	    } else
		memcpy(buf, p->data(), p_length);
	    ring->slot[cur].len = p_length;
	    
	    // need this?
//	__asm__ volatile("" : : : "memory");
	    ring->cur = NETMAP_RING_NEXT(ring, cur);
	    ring->avail--;
	    _my_pkts++;
	    i++;

	    __pbatch_next_pkt(i, pb, _my_port, dport, p);
	}
    }    
    
    if (!_full_nm && i < pb->npkts)
	errno = ENOBUFS;
    return i;
}


int
BToNMDevice::send_packets_nm()
{
    PBatch *p = _q;
    _q = 0;
    int count = 0;

    do {
	if (!p) {
	    _cur = 0;
	    while (!(p = input(0).bpull()) && (++_cur < 10000));
	    if (!p) {
		_cur = 0;
		break;
	    }
	    _cur = 0;
	    _my_pkts = 0;
	}
	
	if ((_cur = netmap_send_batch(p, _cur)) >= p->npkts) {
	    p->kill();
	    p=0;
	    ++count;
	    _cur = 0;
	} else {
	    _q = p;
	    break;
	}
    } while (count < _burst);
    
    return 0;
}

bool
BToNMDevice::run_task(Task *)
{
    int r = 0;
    if (_full_nm) {
	click_chatter("run task of btonmdv t %d\n", click_current_thread_id);
	r = NetmapInfo::run_fd_poll(_nm_fd, _full_nm-1);

	if (r > 0) {
	    _task.fast_reschedule();
	    return true;
	} else
	    return false;
    }

    // Should not go here, use ToNMDevice if not _full_nm.
    
    PBatch *p = _q;
    _q = 0;
    int count = 0;

    do {
	if (!p) {
	    if (!(p = input(0).bpull()))
		break;
	    _cur = 0;
	    _my_pkts = 0;
	}
	if ((_cur = netmap_send_batch(p, _cur)) >= p->npkts) {
	    _backoff = 0;
	    p->kill();
	    ++count;
	    p = 0;
	} else
	    break;
    } while (count < _burst);

    if (p) {
	_q = p;
	if (!_backoff) {
	    _backoff = 1;
	    add_select(_fd, SELECT_WRITE);
	} else {
	    _timer.schedule_after(Timestamp::make_usec(_backoff));
	    if (_backoff < 256)
		_backoff *= 2;
	}
    } else {
	_task.reschedule();
    }       

    return count > 0;
}

void
BToNMDevice::selected(int fd, int mask)
{
    if (_full_nm) {
	send_packets_nm();	
    } else {
	remove_select(_fd, SELECT_WRITE);
	_task.fast_reschedule();
    }
}


String
BToNMDevice::read_param(Element *e, void *thunk)
{
    BToNMDevice *td = (BToNMDevice *)e;
    switch((uintptr_t) thunk) {
    case h_debug:
	return String(td->_debug);
    case h_signal:
	return String(td->_signal);
    case h_pulls:
	return String(td->_pulls);
    case h_q:
	return String((bool) td->_q);
    default:
	return String();
    }
}

int
BToNMDevice::write_param(const String &in_s, Element *e, void *vparam,
		     ErrorHandler *errh)
{
    BToNMDevice *td = (BToNMDevice *)e;
    String s = cp_uncomment(in_s);
    switch ((intptr_t)vparam) {
    case h_debug: {
	bool debug;
	if (!BoolArg().parse(s, debug))
	    return errh->error("type mismatch");
	td->_debug = debug;
	break;
    }
    }
    return 0;
}

void
BToNMDevice::add_handlers()
{
    add_task_handlers(&_task);
    add_read_handler("debug", read_param, h_debug, Handler::CHECKBOX);
    add_read_handler("pulls", read_param, h_pulls);
    add_read_handler("signal", read_param, h_signal);
    add_read_handler("q", read_param, h_q);
    add_write_handler("debug", write_param, h_debug);
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(FromNMDevice userlevel)
EXPORT_ELEMENT(BToNMDevice)
