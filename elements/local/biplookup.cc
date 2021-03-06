#include <click/config.h>
#include <click/error.hh>
#include <click/glue.hh>
#include <click/hvputils.hh>
#include <click/confparse.hh>
#include <click/ipaddress.hh>
#include <click/sync.hh>
#include <click/args.hh>
#include <arpa/inet.h>
#include "biplookup.hh"
CLICK_DECLS

BIPLookup::BIPLookup() : _test(false),
			 _hlpmt(0), _dlpmt(0),
			 _lpmt_lock(0),
			 _lpm_bits(4),
			 _lpm_size(0), _batcher(0)
{
    _anno_offset = -1;
    _slice_offset = -1;
}

BIPLookup::~BIPLookup() {}

int
BIPLookup::configure(Vector<String> &conf, ErrorHandler *errh)
{
    if (!conf.size()) {
	errh->error("No routing entry");
	return -1;
    }

    int nargs;
    if (!cp_integer(conf[0], &nargs)) {
	errh->error("First argument must be integer to specify # of general args");
	return -1;
    }

    Vector<String> rts;
    Vector<String> myconf;

    myconf.reserve(nargs);
    rts.reserve(conf.size());

    int i;
    for (i=1; i<=nargs; i++)
	myconf.push_back(conf[i]);
    for (; i<conf.size(); i++)
	rts.push_back(conf[i]);

    _rtes.reserve(rts.size());

    if (cp_va_kparse(myconf, this, errh,
		     "BATCHER", cpkM, cpElementCast, "Batcher", &_batcher,
		     "TEST", cpkN, cpBool, &_test,
		     "NBITS", cpkN, cpInteger, &_lpm_bits,
		     cpEnd) < 0)
	return -1;
    
    switch(_lpm_bits) {
    case 1:
    case 2:
    case 4:
	break;
    default:
	errh->error("LPM node bits error: %d", _lpm_bits);
	return -1;    
    }

    if (_batcher->req_anno(0, 1, BatchProducer::anno_write)) {
	errh->error("Register annotation request in batcher failed");
	return -1;
    }

    _psr.start = EthernetBatchProducer::ip4_hdr;
    _psr.start_offset = 16; // Dst IP Addr offset
    _psr.len = 4;
    _psr.end = _psr.start+_psr.start_offset+_psr.len;

    if (_batcher->req_slice_range(_psr) < 0) {
	errh->error("Request slice range failed: %d, %d, %d, %d",
		    _psr.start, _psr.start_offset, _psr.len, _psr.end);
	return -1;
    }

    if (IPRouteTable::configure(rts, errh))
	return -1;

    return 0;
}

int
BIPLookup::add_route(const IPRoute& route, bool allow_replace,
		     IPRoute* replaced_route, ErrorHandler* errh)
{
    g4c_ipv4_rt_entry e;
    e.addr = ntohl(route.addr.addr());
    e.mask = ntohl(route.mask.addr());
    e.nnetbits = __builtin_popcount(e.mask);
    e.port = (uint8_t)route.port;

    _rtes.push_back(e);
    return 0;
}

int
BIPLookup::remove_route(const IPRoute& route, IPRoute* removed_route, ErrorHandler* errh)
{
    return -ENOENT;
}

int
BIPLookup::lookup_route(IPAddress addr, IPAddress& gw)
{
    // Not called.
    return 0;
}

String
BIPLookup::dump_routes()
{
    return String("BIPLookup");
}

int
BIPLookup::build_lpmt(vector<g4c_ipv4_rt_entry> &rtes, g4c_lpm_tree *&hlpmt,
		      g4c_lpm_tree *&dlpmt, int nbits, size_t &tsz, ErrorHandler *errh)
{
    g4c_ipv4_rt_entry *ents = new g4c_ipv4_rt_entry[rtes.size()];
    if (!ents) {
	errh->error("Out of memory for RT entries %lu", rtes.size());
        goto err_out;
    }

    memcpy(ents, rtes.data(), sizeof(g4c_ipv4_rt_entry)*rtes.size());
    
    g4c_lpm_tree *t = g4c_build_lpm_tree(ents, rtes.size(), nbits, 0);
    if (!t) {
	errh->error("LPM tree building error");
	goto err_out;
    }

    tsz = sizeof(g4c_lpm_tree);
    switch(nbits) {
    case 1:
	tsz += sizeof(g4c_lpmnode1b_t)*t->nnodes;
	break;
    case 2:
	tsz += sizeof(g4c_lpmnode2b_t)*t->nnodes;
	break;
    case 4:
	tsz += sizeof(g4c_lpmnode4b_t)*t->nnodes;
	break;
    default:
	errh->error("FATAL: %d LPM node bits!", nbits);
	goto err_out;
    }

    hlpmt = (g4c_lpm_tree*)g4c_alloc_page_lock_mem(tsz);
    dlpmt = (g4c_lpm_tree*)g4c_alloc_dev_mem(tsz);
    if (hlpmt && dlpmt) {
	memcpy(hlpmt, t, tsz);
    } else {
	errh->error("Out of mem for lpmt, host %p, dev %p, size %lu.",
		    hlpmt, dlpmt, tsz);
	goto err_out;
    }

    free(t);
    delete[] ents;

    return 0;

err_out:
    if (ents)
	delete[] ents;
    if (t)
	free(t);

    if (hlpmt) {
	g4c_free_host_mem(hlpmt);
	hlpmt = 0;
    }

    if (dlpmt) {
	g4c_free_dev_mem(dlpmt);
	dlpmt = 0;
    }

    return -1;
}

int
BIPLookup::initialize(ErrorHandler *errh)
{
    if (built_lpmt(_rtes, _hlpmt, _dlpmt, _lpm_bits, _lpm_size, errh))
	return -1;

    int s = g4c_alloc_stream();
    if (!s) {
	errh->error("Failed to alloc stream for LPM copy");
	return -1;
    }

    g4c_h2d_async(hlpmt, dlpmt, _lpm_size, s);
    g4c_stream_sync(s);
    g4c_free_stream(s);

    errh->message("LPM tree built and copied to GPU.");

    _anno_offset = _batcher->get_anno_offset(0);
    if (_anno_offset < 0) {
	errh->error("Failed to get anno offset in batch");
	return -1;
    } else
	errh->message("BIPLookup anno offset %d", _anno_offset);

    _slice_offset = _batcher->get_slice_offset(_psr);
    if (_slice_offset < 0) {
	errh->error("Failed to get slice offset in batch");
	return -1;
    } else
	errh->message("BIPLookup slice offset %d", _slice_offset);    
    
    return 0;
}

void
BIPLookup::bpush(int i, PBatch *p)
{
    gpu_ipv4_gpu_lookup_of(_dlpmt,
			   (uint32_t*)p->dslices(), _slice_offset, p->producer->get_slice_stride(),
			   p->dannos(), _anno_offset, p->producer->get_anno_stride(),
			   _lpm_bits, p->npkts, p->dev_stream);
    p->hwork_prt = p->hannos();
    p->dwork_ptr = p->dannos();
    p->work_size = p->npkts * p->producer->get_anno_stride();

    output(0).bpush(p);
}

void
BIPLookup::push(int i, Packet *p)
{
    hvp_chatter("Should never call this: %d, %p\n", i, p);
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(IPRouteTable, Batcher)
EXPORT_ELEMENT(BIPLookup)
ELEMENT_LIBS(-lg4c)
