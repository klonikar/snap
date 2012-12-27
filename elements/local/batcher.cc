#include <click/config.h>
#include <click/error.hh>
#include "batcher.hh"
#include <click/glue.hh>
#include <click/standard/alignmentinfo.hh>
#include <click/hvputils.hh>
#include <click/confparse.hh>
#include <click/pbatch.hh>
#include <click/timestamp.hh>
#include <click/master.hh>
CLICK_DECLS

// Bigger enough to hold batches.
// Doesn't waste much memory, 8 bytes each item.
#define CLICK_PBATCH_POOL_SIZE 65536

Batcher::Batcher(): EthernetBatchProducer(), _timer(this)
{
    _count = 0;
    _drops = 0;
    _batch_capacity = CLICK_PBATCH_CAPACITY;
    _batch = 0;
    _slice_begin = 0;
    _slice_end = 0;
    _anno_begin = 0;
    _anno_end = 0;
    _timeout_ms = CLICK_BATCH_TIMEOUT;
    _force_pktlens = false;
    _timed_batch = 0;
    _test = false;
    _mt_pushers = false;

    _pb_pools = 0;
    _pb_alloc_locks = 0;
    _exp_pb_lock = 0;
    _nr_pools = 0;
    _need_alloc_locking = true;
    _nr_pre_alloc = 10;
}

Batcher::~Batcher()
{
}


int
Batcher::init_pb_pool()
{
    _nr_pools = master()->nthreads()+1;
    if (_nr_pools <= 2 || _mt_pushers == false)
	_need_alloc_locking = false;

    hvp_chatter("Batcher pool, %d pools, %s alloc locking.\n",
		_nr_pools, _need_alloc_locking?"need":"no");

    _pb_pools = new LFRing<PBatch*>[_nr_pools];
    _pb_alloc_locks = new uint32_t[_nr_pools];
    _exp_pb_lock = 0;
    if (_pb_pools && _pb_alloc_locks)
    {
	for (int i=0; i<_nr_pools; i++)
	{
	    if (!_pb_pools[i].reserve(CLICK_PBATCH_POOL_SIZE)) {
		hvp_chatter("Batcher batch pool %d failed to reserve space.", i);
		return -1;
	    }

	    if (_nr_pre_alloc > 0)
	    {
		for (int j=0; j<_nr_pre_alloc; j++) {
		    PBatch* p = this->create_new_batch();
		    if (!p) {
			hvp_chatter("Failed to create %d-th batch for %d pool pre-allocation.",
				    j, i);
			return -1;
		    }
		    
		    init_batch_after_create(p);
		    finit_batch_for_recycle(p);
		    _pb_pools[i].add_new(p);
		}
	    }

	    _pb_alloc_locks[i] = 0;
	}
    }
    else
    {
	hvp_chatter("Out of memory for Batcher batch pools.\n");
	return -1;
    }

    return 0;    
}

PBatch*
Batcher::create_new_batch()
{
    PBatch *pb = new PBatch(this);
    return pb;
}

int
Batcher::init_batch_after_create(PBatch *pb)
{
    pb->init();
    if (this->alloc_batch_priv_data(pb)) {
	hvp_chatter("Batch %p private data failed to alloc.\n", pb);
	return -1;
    }

    if (this->mem_size) {
	void *hm = g4c_alloc_page_lock_mem(this->mem_size);
	void *dm = g4c_alloc_dev_mem(this->mem_size);
	if (hm && dm) {
	    this->assign_batch_mem(pb, hm, dm, this->mem_size);
	} else {
	    hvp_chatter("Mem alloc failed for batch %p, hm %p, "
			"dm %p, sz %lu.\n",
			pb, hm, dm, this->mem_size);
	    return -1;
	}

        pb->hwork_ptr = pb->host_mem;
	pb->dwork_ptr = pb->dev_mem;
	pb->work_size = (int)this->mem_size;
    }
    
    return 0;
}

int
Batcher::init_batch_after_recycle(PBatch *pb)
{       
    return 0;
}

int
Batcher::finit_batch_for_recycle(PBatch *pb)
{
    pb->shared = 0;

    if (pb->dev_stream) {
	g4c_free_stream(pb->dev_stream);
	pb->dev_stream = 0;
    }

    for (int i=0; i<pb->npkts; i++)
	pb->pptrs[i]->kill();
    pb->npkts = 0;

    pb->hwork_ptr = pb->host_mem;
    pb->dwork_ptr = pb->dev_mem;
    pb->work_size = (int)this->mem_size;

    return 0;
}

bool
Batcher::recycle_batch(PBatch *pb)
{
    int tid = click_current_thread_id;
    LFRing<PBatch*> *pool = _pb_pools+tid;
    bool rt = true;

    if (unlikely(tid >= _nr_pools-1)) {
	hvp_chatter("Bad thread id %d catched at recycle batch\n.", tid);
	pool = _pb_pools + (_nr_pools-1);
	if (_nr_pools > 2)
	    while(atomic_uint32_t::swap(_exp_pb_lock, 1) == 1);
    }

    if (unlikely(pool->full())) {
	rt = false;
    } else
	pool->add_new(pb);

    if (unlikely(tid >= _nr_pools-1) && _nr_pools > 2) {
	click_compiler_fence();
	_exp_pb_lock = 0;
    }

    return rt;
}

int
Batcher::destroy_batch(PBatch *pb)
{
    pb->finit();

    if (pb->host_mem)
	g4c_free_page_lock_mem(pb->host_mem);
    if (pb->dev_mem)
	g4c_free_dev_mem(pb->dev_mem);
    delete pb;
    
    return 0;
}

PBatch*
Batcher::alloc_batch()
{
    PBatch *pb = 0;
    int i;//, tid = click_current_thread_id;

    for (int j=0; j<_nr_pools && !pb; ++j)
    {
	i = j;//(tid+j)%_nr_pools;
	if (_need_alloc_locking) {
	    hvp_chatter("locking for pool alloc\n");
	    while(atomic_uint32_t::swap(_pb_alloc_locks[i], 1) == 1);
	}

	if (!_pb_pools[i].empty())
	{
	    pb = _pb_pools[i].remove_and_get_oldest();	    
	}

	if (_need_alloc_locking) {
	    click_compiler_fence();
	    _pb_alloc_locks[i] = 0;
	}
    }

    if (pb) {
	//this->init_batch_after_recycle(pb);
    } else {
	hvp_chatter("Bad we have to create new batch...\n");
	pb = create_new_batch();
	this->init_batch_after_create(pb);
    }

    if (unlikely(_test)) {
	hvp_chatter("Alloc new batch %p\n", pb);
    }

    return pb;
}

int
Batcher::kill_batch(PBatch *pb)
{
    pb->shared--;
    if (pb->shared < 0)
    {
	this->finit_batch_for_recycle(pb);
	if (this->recycle_batch(pb) == false) {
	    this->destroy_batch(pb);
	    return 2;
	}

	return 0;
    }

    return 1; // no kill, shared.
}

/**
 * Pre-condition: _batch exists, _batch not full. Packet p checked.
 */
void
Batcher::add_packet(Packet *p)
{
    int idx = _batch->npkts;

    _batch->npkts++;
    _batch->pptrs[idx] = p;

    const uint8_t *pd_start = p->data();//p->has_mac_header()?p->mac_header():p->data();
    size_t plen = p->end_data() - pd_start;

    if (this->has_lens()) {
	*_batch->length_hptr(idx) = (int16_t)plen;
    }

    if (this->has_annos()) {
	memcpy(_batch->anno_hptr(idx),
	       g4c_ptr_add(p->anno(), this->anno_start),
	       this->anno_len);
    }

    if (this->has_slices())
    {		
	uint8_t *slice = _batch->slice_hptr(idx);
	struct PSliceRange *psr;
	for(int i=0;
	    i<this->nr_slice_ranges;
	    i++)
	{
	    psr = &slice_ranges[i];
	    if ((size_t)(pd_start+psr->start) >= (size_t)(p->end_data()))
		break; // enough, this packet is shorter than expected.
	    
	    memcpy(slice+psr->slice_offset,
		   pd_start+psr->start,
		   psr->len > plen - psr->start? plen-psr->start:psr->len);
	}
    }

    if (idx == 0 && _timeout_ms > 0) {			
	_timer.schedule_after_msec(_timeout_ms);
	_timed_batch = _batch;
    }		
}

void
Batcher::push(int i, Packet *p)
{
    if (!_batch) {
	_batch = alloc_batch();
	if (unlikely(!_batch)) {
	    p->kill();
	    return;
	}	    
    }

    add_packet(p);
    _count++;
	
    if (_batch->npkts >= this->batch_size) {
	if (unlikely(_test)) {
	    hvp_chatter("batch %p full at %s\n", _batch,
			Timestamp::now().unparse().c_str());
	}
	if (unlikely(_timer.scheduled()))
	    _timer.clear();
	PBatch *oldbatch = _batch;
	_batch = alloc_batch();
	output(0).bpush(oldbatch);
    }
}

int
Batcher::configure(Vector<String> &conf, ErrorHandler *errh)
{
    if (cp_va_kparse(conf, this, errh,
		     "TIMEOUT", cpkN, cpInteger, &_timeout_ms,
		     "SLICE_BEGIN", cpkN, cpInteger, &_slice_begin,
		     "SLICE_END", cpkN, cpInteger, &_slice_end,
		     "CAPACITY", cpkN, cpInteger, &_batch_capacity,
		     "ANN_BEGIN", cpkN, cpByte, &_anno_begin,
		     "ANN_END", cpkN, cpByte, &_anno_end,
		     "FORCE_PKTLENS", cpkN, cpBool, &_force_pktlens,
		     "BATCH_PREALLOC", cpkN, cpInteger, &_nr_pre_alloc,
		     "MT_PUSHERS", cpkN, cpBool, &_mt_pushers,
		     "TEST", cpkN, cpBool, &_test,
		     cpEnd) < 0)
	return -1;

    this->set_batch_size(_batch_capacity);
    this->need_lens = _force_pktlens;
    
    if (_slice_end != 0) {
	PSliceRange psr;
	psr.start = _slice_begin;
	psr.start_offset = 0;
	psr.len = _slice_end - _slice_begin;
	psr.end = _slice_end;

	this->req_slice_range(psr);
    }

    if (_anno_end != 0) {
	this->req_anno(_anno_begin, _anno_end-_anno_begin, anno_write|anno_read);
    }
    
    return 0;
}

int
Batcher::initialize(ErrorHandler *errh)
{
    _timer.initialize(this);

    this->setup_all();
    if (!init_pb_pool()) {
	hvp_chatter("Batch pool initialized.\n");
    } else
	return -1;
    
    return 0;
}

void
Batcher::run_timer(Timer *timer)
{
    PBatch *pb = _timed_batch;
    if (pb != _batch || !pb)
	return;

    _batch = alloc_batch();
    if (_test)
	hvp_chatter("batch %p(%d) timeout at %s\n", pb, pb->npkts,
		    Timestamp::now().unparse().c_str());
    output(0).bpush(pb);
}

CLICK_ENDDECLS
EXPORT_ELEMENT(Batcher)
ELEMENT_LIBS(-lg4c)

