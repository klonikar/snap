#include <click/config.h>
#include "nmtodevice.hh"
#include <click/error.hh>
#include <click/etheraddress.hh>
#include <click/args.hh>
#include <click/router.hh>
#include <click/standard/scheduleinfo.hh>
#include <click/packet_anno.hh>
#include <click/straccum.hh>
#include <stdio.h>
#include <unistd.h>

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

NMToDevice::NMToDevice()
    : _task(this), _timer(&_task), _q(0), _pulls(0)
{
    _fd = -1;
    _my_fd = false;
    _ringid = -1;
}

NMToDevice::~NMToDevice()
{
}

int
NMToDevice::configure(Vector<String> &conf, ErrorHandler *errh)
{
    _burst = 1;
    _ringid = -1;
    if (Args(conf, this, errh)
	.read_mp("DEVNAME", _ifname)
	.read("DEBUG", _debug)
	.read("BURST", _burst)
	.read("RING", _ringid)
	.complete() < 0)
	return -1;
    if (!_ifname)
	return errh->error("interface not set");
    if (_burst <= 0)
	return errh->error("bad BURST");

    return 0;
}

NMFromDevice *
NMToDevice::find_nmfromdevice() const
{
    Router *r = router();
    for (int ei = 0; ei < r->nelements(); ++ei) {
	NMFromDevice *fd = (NMFromDevice *) r->element(ei)->cast("NMFromDevice");
	if (fd && fd->ifname() == _ifname
	    && fd->dev_ringid() == _ringid
	    && fd->fd() >= 0)
	    return fd;
    }
    return 0;
}

int
NMToDevice::initialize(ErrorHandler *errh)
{
    _timer.initialize(this);

    FromDevice *fd = find_fromdevice();
    if (fd && fd->netmap()) {
	_fd = fd->fd();
	_netmap = *fd->netmap();
    } else {
	if (_ringid >= 0)
	    _fd = _netmap.open_ring(_ifname, _ringid,
				    1, errh);
	else
	    _fd = _netmap.open(_ifname, 1, errh);
	if (_fd >= 0) {
	    _my_fd = true;
	    add_select(_fd, SELECT_READ); // NB NOT writable!
	} else
	    return -1;
    }
    if (_fd >= 0) {
	_netmap.initialize_rings_tx();
    }

    // check for duplicate writers
    void *&used = router()->force_attachment("device_writer_" + _ifname);
    if (used)
	return errh->error("duplicate writer for device %<%s%>", _ifname.c_str());
    used = this;

    ScheduleInfo::join_scheduler(this, &_task, errh);
    _signal = Notifier::upstream_empty_signal(this, 0, &_task);
    return 0;
}

void
NMToDevice::cleanup(CleanupStage)
{
    if (_fd >= 0 && _my_fd) {
	_netmap.close(_fd);
	_fd = -1;
    }
}


int
NMToDevice::netmap_send_packet(Packet *p)
{
    for (unsigned ri = _netmap.ring_begin; ri != _netmap.ring_end; ++ri) {
	struct netmap_ring *ring = NETMAP_TXRING(_netmap.nifp, ri);
	if (ring->avail == 0)
	    continue;
	unsigned cur = ring->cur;
	unsigned buf_idx = ring->slot[cur].buf_idx;
	if (buf_idx < 2)
	    continue;
	unsigned char *buf = (unsigned char *) NETMAP_BUF(ring, buf_idx);
	uint32_t p_length = p->length();
	if (NetmapInfo::is_netmap_buffer(p)
	    && !p->shared() && p->buffer() == p->data()
	    && noutputs() == 0) {
	    ring->slot[cur].buf_idx = NETMAP_BUF_IDX(ring, (char *) p->buffer());
	    ring->slot[cur].flags |= NS_BUF_CHANGED;
	    NetmapInfo::buffer_destructor(buf, 0);
	    p->reset_buffer();
	} else
	    memcpy(buf, p->data(), p_length);
	ring->slot[cur].len = p_length;
	__asm__ volatile("" : : : "memory");
	ring->cur = NETMAP_RING_NEXT(ring, cur);
	ring->avail--;
	return 0;
    }
    errno = ENOBUFS;
    return -1;
}

/*
 * Linux select marks datagram fd's as writeable when the socket
 * buffer has enough space to do a send (sock_writeable() in
 * sock.h). BSD select always marks datagram fd's as writeable
 * (bpf_poll() in sys/net/bpf.c) This function should behave
 * appropriately under both.  It makes use of select if it correctly
 * tells us when buffers are available, and it schedules a backoff
 * timer if buffers are not available.
 * --jbicket
 */
int
NMToDevice::send_packet(Packet *p)
{
    int r = 0;
    errno = 0;

    r = netmap_send_packet(p);

    if (r >= 0)
	return 0;
    else
	return errno ? -errno : -EINVAL;
}

bool
NMToDevice::run_task(Task *)
{
    Packet *p = _q;
    _q = 0;
    int count = 0, r = 0;

    do {
	if (!p) {
	    ++_pulls;
	    if (!(p = input(0).pull()))
		break;
	}
	if ((r = send_packet(p)) >= 0) {
	    _backoff = 0;
	    checked_output_push(0, p);
	    ++count;
	    p = 0;
	} else
	    break;
    } while (count < _burst);

    if (r == -ENOBUFS || r == -EAGAIN) {
	assert(!_q);
	_q = p;

	if (!_backoff) {
	    _backoff = 1;
	    add_select(_fd, SELECT_WRITE);
	} else {
	    _timer.schedule_after(Timestamp::make_usec(_backoff));
	    if (_backoff < 256)
		_backoff *= 2;
	    if (_debug) {
		Timestamp now = Timestamp::now();
		click_chatter("%p{element} backing off for %d at %p{timestamp}\n", this, _backoff, &now);
	    }
	}
	return count > 0;
    } else if (r < 0) {
	click_chatter("NMToDevice(%s): %s", _ifname.c_str(), strerror(-r));
	checked_output_push(1, p);
    }

    if (p || _signal)
	_task.fast_reschedule();
    return count > 0;
}

void
NMToDevice::selected(int, int)
{
    _task.reschedule();
    remove_select(_fd, SELECT_WRITE);
}


String
NMToDevice::read_param(Element *e, void *thunk)
{
    NMToDevice *td = (NMToDevice *)e;
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
NMToDevice::write_param(const String &in_s, Element *e, void *vparam,
		     ErrorHandler *errh)
{
    NMToDevice *td = (NMToDevice *)e;
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
NMToDevice::add_handlers()
{
    add_task_handlers(&_task);
    add_read_handler("debug", read_param, h_debug, Handler::CHECKBOX);
    add_read_handler("pulls", read_param, h_pulls);
    add_read_handler("signal", read_param, h_signal);
    add_read_handler("q", read_param, h_q);
    add_write_handler("debug", write_param, h_debug);
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(NMFromDevice userlevel)
EXPORT_ELEMENT(NMToDevice)
