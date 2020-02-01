/*
 * Copyright (c) 2012-2014, 2017 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Andrew Bardsley
 */

#include "cpu/minor/cpu.hh"

#include "arch/utility.hh"
#include "cpu/minor/dyn_inst.hh"
#include "cpu/minor/fetch1.hh"
#include "cpu/minor/pipeline.hh"
#include "debug/Drain.hh"
#include "debug/MinorCPU.hh"
#include "debug/Quiesce.hh"
#include "mem/ruby/slicc_interface/RubySlicc_ComponentMapping.hh"
#include "mem/protocol/MachineType.hh"
#include "mem/ruby/slicc_interface/RubySlicc_Util.hh"

#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/network/Network.hh"
#include "mem/ruby/network/simple/SimpleNetwork.hh"

MinorCPU::MinorCPU(MinorCPUParams *params) :
    BaseCPU(params),
	Consumer(this),
    pipelineStartupEvent([this]{ wakeupPipeline(); }, name()),
    threadPolicy(params->threadPolicy)
{
    /* This is only written for one thread at the moment */
    Minor::MinorThread *thread;

    for (ThreadID i = 0; i < numThreads; i++) {
        if (FullSystem) {
            thread = new Minor::MinorThread(this, i, params->system,
                    params->itb, params->dtb, params->isa[i]);
            thread->setStatus(ThreadContext::Halted);
        } else {
            thread = new Minor::MinorThread(this, i, params->system,
                    params->workload[i], params->itb, params->dtb,
                    params->isa[i]);
        }

        threads.push_back(thread);
        ThreadContext *tc = thread->getTC();
        threadContexts.push_back(tc);
    }


    if (params->checker) {
        fatal("The Minor model doesn't support checking (yet)\n");
    }

    Minor::MinorDynInst::init();

    pipeline = new Minor::Pipeline(*this, *params);
    activityRecorder = pipeline->getActivityRecorder();

	// does it work like get()?
	// printf("Number of accel in the system are: %d\n",params->numThreads);
	requestFromSpu = params->requestFromSpu;
	responseToSpu = params->responseToSpu;
	dummy1 = params->dummy1;
	dummy2 = params->dummy2;
	dummy3 = params->dummy3;

	// FIXME: might need to add this (let's keep it added actually)
	// will be useful for getDest, etc
    createMachineID(MachineType_Accel, intToID(cpuId()));
	// TODO: check difference between init and constructor
	(*responseToSpu).setConsumer(this);
}

bool MinorCPU::check_network_idle() {
  if(!requestFromSpu->isEmpty()) return false;
  if(!responseToSpu->isEmpty()) return false;
  return (SimpleNetwork*)(spu_net_ptr)->internal_links_idle();
}

void MinorCPU::wakeup()
{
  // TODO: while used when multiple packets may be received (works when only
  // 1 packet issued per cycle)
  // if(!responseToSpu->isEmpty()) {
  while(!responseToSpu->isEmpty()) { // same packet received from different cores?

    if(!responseToSpu->isReady(clockEdge())) return;
    const SpuRequestMsg* msg = (SpuRequestMsg*)responseToSpu->peek();
   // for global barrier
    ThreadContext *thread = getContext(0); // assume tid=0?
    thread->getSystemPtr()->inc_spu_receive();

    // could do dynamic cast
    int64_t return_info = msg->m_addr;

    int num_bytes = return_info >> 16;
    int8_t data[num_bytes];

    // FIXME: may not be applicable for others too (now I have move to
    // delimeter way)
    if((*msg).m_Type != SpuRequestType_UPDATE) {
      for(int i=0; i<num_bytes; ++i) {
        data[i] = (*msg).m_DataBlk.getByte(i);
      }
    }
    if(SS_DEBUG::NET_REQ){
      // timestamp();
      std::cout << curCycle();
      printf("Wake up accel at destination node: %d and num_bytes: %d and complete return info: %ld\n",cpuId(),num_bytes, return_info);
    }

    if((*msg).m_Type == SpuRequestType_UPDATE) {
      bool is_tagged = return_info & 1; 
      bool is_tag_packet = (return_info >> 1) & 1; 
      // std::cout << "update request received, is tagged: " << is_tagged << " is tag packet: " << is_tag_packet << "\n";
      // Step1: get the start address from here
      if(is_tagged) {
        int tag = (return_info >> 2) & 65535;
        // std::cout << "tag in the received packet: " << tag << "\n";
        uint8_t l;
        if(is_tag_packet) {
          int bytes_waiting = (return_info >> 18);
          std::vector<int> start_addr;
          uint64_t inc = 0;
          // TODO: I need to split which of these addresses belong to the
          // current node!!!
          for(int i=0; i<60; i+=3) { // limit on this?
            inc = 0;
            for(int k=0; k<3; k++) { // 32768
              int8_t x = msg->m_DataBlk.getByte(i+k);
              if(signed(x)==-1) {
                i=60; break;
              }
              l=x;
              inc = inc | (l << k*8);
              // std::cout << "8-bit value: " << (signed)(x) << "\n";
            }
            // std::cout << "i: " << i << " inc: " << inc << std::endl;
            if(i!=60 && (inc/SCRATCH_SIZE==cpuId()-1)) {
              start_addr.push_back(inc & (SCRATCH_SIZE-1));
            }
          }
          // std::cout << "core: " << cpuId() << " addr received: " << start_addr.size() << " and bytes waiting for each: " << bytes_waiting << "\n";
          pipeline->insert_pending_request_queue(tag, start_addr, bytes_waiting);
        } else {
          std::vector<uint8_t> inc_val;
          for(int i=0; i<64; ++i) {
            int8_t x = msg->m_DataBlk.getByte(i);
            if(signed(x)==-1) break;
            l=x;
            inc_val.push_back(l);
          }
          // std::cout << "number of value bytes received: " << inc_val.size();
          int num_addr = pipeline->push_and_update_addr_in_pq(tag, inc_val.size());
          // std::cout << " num addr waiting to consume all values: " << num_addr << "\n";
          // pop values and push in the value queue (num_times is start addr)
          // ssim.push_atomic_inc(inc_val, num_addr); // vec of values, repeat times
          pipeline->push_atomic_inc(inc_val, num_addr);
        }
      } else { // TODO: like the old way to pushing both together


      }


      /*
      // Step2: push values and addresses to its corresponding ports (copy
      // duplicate data for addr mix and val)
      // TODO: need all the info to push into banks
      int opcode = (return_info >> 16) & 3;
      int val_bytes = (return_info >> 18) & 3;
      int out_bytes = (return_info >> 20) & 3;
      int scr_addr = return_info & 65535;


        if(SS_DEBUG::NET_REQ) {
        std::cout << "Received atomic update request tuple, scr_addr: " << scr_addr << " opcode: " << opcode << " val_bytes: " << val_bytes << " out_bytes: " << out_bytes << std::endl;
      }
      // pipeline->receiveSpuUpdateRequest(scr_addr, opcode, val_bytes, out_bytes, inc);
      // FIXME:IMP: allocate more bits to specify datatype
      pipeline->receiveSpuUpdateRequest(scr_addr, opcode, 8, 8, inc);
*/


    } else if((*msg).m_Type == SpuRequestType_LD) { 
      // TODO: read these values from the block
      // if read request, so this will push in bank queues and read data
      int8_t x = (*msg).m_DataBlk.getByte(0);
      bool read_req = (signed(x)==-1);
      int addr = return_info && 65535;
      int request_ptr = (return_info >> 16) & 63; 
      int data_bytes = (return_info >> 22) & 15;
      int reorder_entry = (return_info >> 26) & 7;

      if(data_bytes>8) data_bytes=NUM_SCRATCH_BANKS*(data_bytes-8);

      if(SS_DEBUG::NET_REQ) {
        std::cout << " Request: " << read_req << "\n";
        std::cout << "In wakeup, remote read with addr: " << addr << " x dim: " << request_ptr << " y dim: " << reorder_entry << " and data bytes: " << data_bytes << std::endl;
        }
      if(read_req) {
        int req_core=0;
        for(int j=1; j<6; ++j) {
          uint8_t b = (*msg).m_DataBlk.getByte(j);
          req_core = req_core | (b << ((j-1)*8));
        }
        if(SS_DEBUG::NET_REQ) {
          std::cout << "Read request with req core: " << req_core << std::endl;
        }
        // should only use the local addr instead of global location
        addr = addr & (SCRATCH_SIZE-1);
        pipeline->receiveSpuReadRequest(req_core, request_ptr, addr, data_bytes, reorder_entry);
      } else {
        // for this directly push in the irob
        pipeline->receiveSpuReadData(data, request_ptr, addr, data_bytes, reorder_entry);
      }
    } else if((*msg).m_Type == SpuRequestType_ST) {
      int remote_port_id = return_info & 63;
      if(SS_DEBUG::NET_REQ) {
        std::cout << "Received multicast message at remote port: " << remote_port_id << " with number of bytes: " << num_bytes << std::endl;
      }
      pipeline->receiveSpuMessage(data, num_bytes, remote_port_id);
    }
    else {
      assert(0 && "unknown SPU message type");
    }
    responseToSpu->dequeue(clockEdge());
  };
}

void MinorCPU::print(std::ostream& out) const
{
  out << "[CPU " << cpuId() << "]";
}


MinorCPU::~MinorCPU()
{
    delete pipeline;

    for (ThreadID thread_id = 0; thread_id < threads.size(); thread_id++) {
        delete threads[thread_id];
    }
}

void
MinorCPU::init()
{
    BaseCPU::init();

    if (!params()->switched_out &&
        system->getMemoryMode() != Enums::timing)
    {
        fatal("The Minor CPU requires the memory system to be in "
            "'timing' mode.\n");
    }

    /* Initialise the ThreadContext's memory proxies */
    for (ThreadID thread_id = 0; thread_id < threads.size(); thread_id++) {
        ThreadContext *tc = getContext(thread_id);

        tc->initMemProxies(tc);
    }

    /* Initialise CPUs (== threads in the ISA) */
    if (FullSystem && !params()->switched_out) {
        for (ThreadID thread_id = 0; thread_id < threads.size(); thread_id++)
        {
            ThreadContext *tc = getContext(thread_id);

            /* Initialize CPU, including PC */
            TheISA::initCPU(tc, cpuId());
        }
    }

}

/** Stats interface from SimObject (by way of BaseCPU) */
void
MinorCPU::regStats()
{
    BaseCPU::regStats();
    stats.regStats(name(), *this);
    pipeline->regStats();
}

void
MinorCPU::serializeThread(CheckpointOut &cp, ThreadID thread_id) const
{
    threads[thread_id]->serialize(cp);
}

void
MinorCPU::unserializeThread(CheckpointIn &cp, ThreadID thread_id)
{
    threads[thread_id]->unserialize(cp);
}

void
MinorCPU::serialize(CheckpointOut &cp) const
{
    pipeline->serialize(cp);
    BaseCPU::serialize(cp);
}

void
MinorCPU::unserialize(CheckpointIn &cp)
{
    pipeline->unserialize(cp);
    BaseCPU::unserialize(cp);
}

Addr
MinorCPU::dbg_vtophys(Addr addr)
{
    /* Note that this gives you the translation for thread 0 */
    panic("No implementation for vtophy\n");

    return 0;
}

void
MinorCPU::wakeup(ThreadID tid)
{
    DPRINTF(Drain, "[tid:%d] MinorCPU wakeup\n", tid);
    assert(tid < numThreads);

    if (threads[tid]->status() == ThreadContext::Suspended) {
        threads[tid]->activate();
    }
}

void
MinorCPU::startup()
{
    DPRINTF(MinorCPU, "MinorCPU startup\n");

    BaseCPU::startup();

    for (ThreadID tid = 0; tid < numThreads; tid++) {
        threads[tid]->startup();
        pipeline->wakeupFetch(tid);
    }
}

DrainState
MinorCPU::drain()
{
    // Deschedule any power gating event (if any)
    deschedulePowerGatingEvent();

    if (switchedOut()) {
        DPRINTF(Drain, "Minor CPU switched out, draining not needed.\n");
        return DrainState::Drained;
    }

    DPRINTF(Drain, "MinorCPU drain\n");

    /* Need to suspend all threads and wait for Execute to idle.
     * Tell Fetch1 not to fetch */
    if (pipeline->drain()) {
        DPRINTF(Drain, "MinorCPU drained\n");
        return DrainState::Drained;
    } else {
        DPRINTF(Drain, "MinorCPU not finished draining\n");
        return DrainState::Draining;
    }
}

void
MinorCPU::signalDrainDone()
{
    DPRINTF(Drain, "MinorCPU drain done\n");
    Drainable::signalDrainDone();
}

void
MinorCPU::drainResume()
{
    /* When taking over from another cpu make sure lastStopped
     * is reset since it might have not been defined previously
     * and might lead to a stats corruption */
    pipeline->resetLastStopped();

    if (switchedOut()) {
        DPRINTF(Drain, "drainResume while switched out.  Ignoring\n");
        return;
    }

    DPRINTF(Drain, "MinorCPU drainResume\n");

    if (!system->isTimingMode()) {
        fatal("The Minor CPU requires the memory system to be in "
            "'timing' mode.\n");
    }

    for (ThreadID tid = 0; tid < numThreads; tid++){
        wakeup(tid);
    }

    pipeline->drainResume();

    // Reschedule any power gating event (if any)
    schedulePowerGatingEvent();
}

void
MinorCPU::memWriteback()
{
    DPRINTF(Drain, "MinorCPU memWriteback\n");
}

void
MinorCPU::switchOut()
{
    DPRINTF(MinorCPU, "MinorCPU switchOut\n");

    assert(!switchedOut());
    BaseCPU::switchOut();

    /* Check that the CPU is drained? */
    activityRecorder->reset();
}

void
MinorCPU::takeOverFrom(BaseCPU *old_cpu)
{
    DPRINTF(MinorCPU, "MinorCPU takeOverFrom\n");

    BaseCPU::takeOverFrom(old_cpu);
}

void
MinorCPU::activateContext(ThreadID thread_id)
{
    // DPRINTF(MinorCPU, "ActivateContext thread: %d\n", thread_id);
   /* Remember to wake up this thread_id by scheduling the
     * pipelineStartup event.
     * We can't wakeupFetch the thread right away because its context may
     * not have been fully initialized. For example, in the case of clone
     * syscall, this activateContext function is called in the middle of
     * the syscall and before the new thread context is initialized.
     * If we start fetching right away, the new thread will fetch from an
     * invalid address (i.e., pc is not initialized yet), which could lead
     * to a page fault. Instead, we remember which threads to wake up and
     * schedule an event to wake all them up after their contexts are
     * fully initialized */
    readyThreads.push_back(thread_id);
    if (!pipelineStartupEvent.scheduled())
      schedule(pipelineStartupEvent, clockEdge(Cycles(0)));
}
    /* Do some cycle accounting.  lastStopped is reset to stop the
     *  wakeup call on the pipeline from adding the quiesce period
     *  to BaseCPU::numCycles */
    // stats.quiesceCycles += pipeline->cyclesSinceLastStopped();
    // pipeline->resetLastStopped();

    /* Wake up the thread, wakeup the pipeline tick */
    // threads[thread_id]->activate();
    // wakeupOnEvent(Minor::Pipeline::CPUStageId);
    // pipeline->wakeupFetch(thread_id);

    // BaseCPU::activateContext(thread_id);
// }
void
MinorCPU::wakeupPipeline()
{
    for (auto thread_id : readyThreads) {
        DPRINTF(MinorCPU, "ActivateContext thread: %d\n", thread_id);
         /* Do some cycle accounting.  lastStopped is reset to stop the
         *  wakeup call on the pipeline from adding the quiesce period
         *  to BaseCPU::numCycles */
        stats.quiesceCycles += pipeline->cyclesSinceLastStopped();
        pipeline->resetLastStopped();

    // BaseCPU::activateContext(thread_id);
        /* Wake up the thread, wakeup the pipeline tick */
        threads[thread_id]->activate();
        wakeupOnEvent(Minor::Pipeline::CPUStageId);

        pipeline->wakeupFetch(thread_id);
        BaseCPU::activateContext(thread_id);
    }

    readyThreads.clear();
}

void
MinorCPU::suspendContext(ThreadID thread_id)
{
    DPRINTF(MinorCPU, "SuspendContext %d\n", thread_id);

    threads[thread_id]->suspend();

    BaseCPU::suspendContext(thread_id);
}

void
MinorCPU::wakeupOnEvent(unsigned int stage_id)
{
    DPRINTF(Quiesce, "Event wakeup from stage %d\n", stage_id);

    /* Mark that some activity has taken place and start the pipeline */
    activityRecorder->activateStage(stage_id);
    pipeline->start();
}

MinorCPU *
MinorCPUParams::create()
{
    return new MinorCPU(this);
}

MasterPort &MinorCPU::getInstPort()
{
    return pipeline->getInstPort();
}

MasterPort &MinorCPU::getDataPort()
{
    return pipeline->getDataPort();
}

Counter
MinorCPU::totalInsts() const
{
    Counter ret = 0;

    for (auto i = threads.begin(); i != threads.end(); i ++)
        ret += (*i)->numInst;

    return ret;
}

Counter
MinorCPU::totalOps() const
{
    Counter ret = 0;

    for (auto i = threads.begin(); i != threads.end(); i ++)
        ret += (*i)->numOp;

    return ret;
}
