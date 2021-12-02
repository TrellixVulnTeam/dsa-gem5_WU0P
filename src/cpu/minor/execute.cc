/*
 * Copyright (c) 2013-2014,2018-2019 ARM Limited
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
 */

#include "cpu/minor/execute.hh"

#include "arch/locked_mem.hh"
#include "arch/registers.hh"
#include "arch/utility.hh"
#include "cpu/minor/cpu.hh"
#include "cpu/minor/exec_context.hh"
#include "cpu/minor/fetch1.hh"
#include "cpu/minor/lsq.hh"
#include "cpu/op_class.hh"
#include "debug/Activity.hh"
#include "debug/Branch.hh"
#include "debug/Drain.hh"
#include "debug/MinorExecute.hh"
#include "debug/MinorInterrupt.hh"
#include "debug/MinorMem.hh"
#include "debug/MinorTrace.hh"
#include "debug/PCEvent.hh"
#include <bits/stdc++.h>

#include "mem/ruby/protocol/SequencerMsg.hh"
#include "mem/ruby/protocol/SpuRequestMsg.hh"
#include "mem/ruby/slicc_interface/RubySlicc_Util.hh"
#include "mem/ruby/network/Network.hh"
#include "mem/ruby/system/RubySystem.hh"
// #include "mem/protocol/SequencerRequestType.hh"

#include "ssim/sim-debug.hh"

namespace Minor
{

Execute::Execute(const std::string &name_,
    MinorCPU &cpu_,
    MinorCPUParams &params,
    Latch<ForwardInstData>::Output inp_,
    Latch<BranchData>::Input out_) :
    Named(name_),
    inp(inp_),
    out(out_),
    cpu(cpu_),
    issueLimit(params.executeIssueLimit),
    memoryIssueLimit(params.executeMemoryIssueLimit),
    commitLimit(params.executeCommitLimit),
    memoryCommitLimit(params.executeMemoryCommitLimit),
    processMoreThanOneInput(params.executeCycleInput),
    fuDescriptions(*params.executeFuncUnits),
    numFuncUnits(fuDescriptions.funcUnits.size()),
    setTraceTimeOnCommit(params.executeSetTraceTimeOnCommit),
    setTraceTimeOnIssue(params.executeSetTraceTimeOnIssue),
    allowEarlyMemIssue(params.executeAllowEarlyMemoryIssue),
    noCostFUIndex(fuDescriptions.funcUnits.size() + 1),
    // noCostFUIndex(16),
    lsq(name_ + ".lsq", name_ + ".dcache_port",
        cpu_, *this,
        params.executeMaxAccessesInMemory,
        params.executeMemoryWidth,
        params.executeLSQRequestsQueueSize,
        params.executeLSQTransfersQueueSize,
        params.executeLSQStoreBufferSize,
        params.executeLSQMaxStoreBufferStoresPerCycle),
    ssim(&lsq),
    executeInfo(params.numThreads, ExecuteThreadInfo(params.executeCommitLimit)),
    interruptPriority(0),
    issuePriority(0),
    commitPriority(0)
{
    if (commitLimit < 1) {
        fatal("%s: executeCommitLimit must be >= 1 (%d)\n", name_,
            commitLimit);
    }

    if (issueLimit < 1) {
        fatal("%s: executeCommitLimit must be >= 1 (%d)\n", name_,
            issueLimit);
    }

    if (memoryIssueLimit < 1) {
        fatal("%s: executeMemoryIssueLimit must be >= 1 (%d)\n", name_,
            memoryIssueLimit);
    }

    if (memoryCommitLimit > commitLimit) {
        fatal("%s: executeMemoryCommitLimit (%d) must be <="
            " executeCommitLimit (%d)\n",
            name_, memoryCommitLimit, commitLimit);
    }

    if (params.executeInputBufferSize < 1) {
        fatal("%s: executeInputBufferSize must be >= 1 (%d)\n", name_,
        params.executeInputBufferSize);
    }

    if (params.executeInputBufferSize < 1) {
        fatal("%s: executeInputBufferSize must be >= 1 (%d)\n", name_,
        params.executeInputBufferSize);
    }

    /* This should be large enough to count all the in-FU instructions
     *  which need to be accounted for in the inFlightInsts
     *  queue */
    unsigned int total_slots = 0;

    /* Make FUPipelines for each MinorFU */
    for (unsigned int i = 0; i < numFuncUnits; i++) {
        std::ostringstream fu_name;
        MinorFU *fu_description = fuDescriptions.funcUnits[i];

        /* Note the total number of instruction slots (for sizing
         *  the inFlightInst queue) and the maximum latency of any FU
         *  (for sizing the activity recorder) */
        total_slots += fu_description->opLat;

        fu_name << name_ << ".fu." << i;

        FUPipeline *fu = new FUPipeline(fu_name.str(), *fu_description, cpu);

        funcUnits.push_back(fu);
    }

    /** Check that there is a functional unit for all operation classes */
    for (int op_class = No_OpClass + 1; op_class < Num_OpClasses; op_class++) {
        bool found_fu = false;
        unsigned int fu_index = 0;

        while (fu_index < numFuncUnits && !found_fu)
        {
            if (funcUnits[fu_index]->provides(
                static_cast<OpClass>(op_class)))
            {
                found_fu = true;
            }
            fu_index++;
        }

        if (!found_fu) {
            warn("No functional unit for OpClass %s\n",
                Enums::OpClassStrings[op_class]);
        }
    }

    /* Per-thread structures */
    for (ThreadID tid = 0; tid < params.numThreads; tid++) {
        std::string tid_str = std::to_string(tid);

        /* Input Buffers */
        inputBuffer.push_back(
            InputBuffer<ForwardInstData>(
                name_ + ".inputBuffer" + tid_str, "insts",
                params.executeInputBufferSize));

        /* Scoreboards */
        scoreboard.push_back(Scoreboard(name_ + ".scoreboard" + tid_str));

        /* In-flight instruction records */
        executeInfo[tid].inFlightInsts =  new Queue<QueuedInst,
            ReportTraitsAdaptor<QueuedInst> >(
            name_ + ".inFlightInsts" + tid_str, "insts", total_slots);

        executeInfo[tid].inFUMemInsts = new Queue<QueuedInst,
            ReportTraitsAdaptor<QueuedInst> >(
            name_ + ".inFUMemInsts" + tid_str, "insts", total_slots);
    }
}



void
Execute::SpuRequest::makePacket()
{
    /* Make the necessary packet for a memory transaction */
    packet = new Packet(request, MemCmd::ReadReq);
    packet->allocate();

    /* This FetchRequest becomes SenderState to allow the response to be
     *  identified */
    packet->pushSenderState(this);
}

/*
void
Execute::SpuRequest::finish(const Fault &fault_, const RequestPtr &request_,
                             ThreadContext *tc, BaseTLB::Mode mode)
{
    fault = fault_;

    state = Translated;
    // fetch.handleTLBResponse(this);

    // Let's try and wake up the processor for the next cycle
	// FIXME: do I need this?
    // execute.cpu.wakeupOnEvent(Pipeline::Fetch1StageId);
}
*/

Execute::SpuRequest::~SpuRequest()
{
    if (packet)
        delete packet;
}

void
Execute::SpuRequest::reportData(std::ostream &os) const
{
    ////  os << id;
}

// TODO: check if this is important
bool Execute::SpuRequest::isDiscardable() const
{
  return false;
    // Fetch1ThreadInfo &thread = fetch.fetchInfo[id.threadId];

    // /* Can't discard lines in TLB/memory */
    // return state != InTranslation && state != RequestIssuing &&
    //     (id.streamSeqNum != thread.streamSeqNum ||
    //     id.predictionSeqNum != thread.predictionSeqNum);
}



const ForwardInstData *
Execute::getInput(ThreadID tid)
{
    /* Get a line from the inputBuffer to work with */
    if (!inputBuffer[tid].empty()) {
        const ForwardInstData &head = inputBuffer[tid].front();

        return (head.isBubble() ? NULL : &(inputBuffer[tid].front()));
    } else {
        return NULL;
    }
}

void
Execute::popInput(ThreadID tid)
{
    if (!inputBuffer[tid].empty())
        inputBuffer[tid].pop();

    executeInfo[tid].inputIndex = 0;
}

void
Execute::tryToBranch(MinorDynInstPtr inst, Fault fault, BranchData &branch)
{
    ThreadContext *thread = cpu.getContext(inst->id.threadId);
    const TheISA::PCState &pc_before = inst->pc;
    TheISA::PCState target = thread->pcState();

    /* Force a branch for SerializeAfter/SquashAfter instructions
     * at the end of micro-op sequence when we're not suspended */
    bool force_branch = thread->status() != ThreadContext::Suspended &&
        !inst->isFault() &&
        inst->isLastOpInInst() &&
        (inst->staticInst->isSerializeAfter() ||
         inst->staticInst->isSquashAfter() ||
         inst->staticInst->isIprAccess());

    DPRINTF(Branch, "tryToBranch before: %s after: %s%s\n",
        pc_before, target, (force_branch ? " (forcing)" : ""));

    /* Will we change the PC to something other than the next instruction? */
    bool must_branch = pc_before != target ||
        fault != NoFault ||
        force_branch;

    /* The reason for the branch data we're about to generate, set below */
    BranchData::Reason reason = BranchData::NoBranch;

    if (fault == NoFault)
    {
        TheISA::advancePC(target, inst->staticInst);
        thread->pcState(target);

        DPRINTF(Branch, "Advancing current PC from: %s to: %s\n",
            pc_before, target);
    }

    if (inst->predictedTaken && !force_branch) {
        /* Predicted to branch */
        if (!must_branch) {
            /* No branch was taken, change stream to get us back to the
             *  intended PC value */
            DPRINTF(Branch, "Predicted a branch from 0x%x to 0x%x but"
                " none happened inst: %s\n",
                inst->pc.instAddr(), inst->predictedTarget.instAddr(), *inst);

            reason = BranchData::BadlyPredictedBranch;
        } else if (inst->predictedTarget == target) {
            /* Branch prediction got the right target, kill the branch and
             *  carry on.
             *  Note that this information to the branch predictor might get
             *  overwritten by a "real" branch during this cycle */
            DPRINTF(Branch, "Predicted a branch from 0x%x to 0x%x correctly"
                " inst: %s\n",
                inst->pc.instAddr(), inst->predictedTarget.instAddr(), *inst);

            reason = BranchData::CorrectlyPredictedBranch;
        } else {
            /* Branch prediction got the wrong target */
            DPRINTF(Branch, "Predicted a branch from 0x%x to 0x%x"
                    " but got the wrong target (actual: 0x%x) inst: %s\n",
                    inst->pc.instAddr(), inst->predictedTarget.instAddr(),
                    target.instAddr(), *inst);

            reason = BranchData::BadlyPredictedBranchTarget;
        }
    } else if (must_branch) {
        /* Unpredicted branch */
        DPRINTF(Branch, "Unpredicted branch from 0x%x to 0x%x inst: %s\n",
            inst->pc.instAddr(), target.instAddr(), *inst);

        reason = BranchData::UnpredictedBranch;
    } else {
        /* No branch at all */
        reason = BranchData::NoBranch;
    }

    updateBranchData(inst->id.threadId, reason, inst, target, branch);
}

void
Execute::updateBranchData(
    ThreadID tid,
    BranchData::Reason reason,
    MinorDynInstPtr inst, const TheISA::PCState &target,
    BranchData &branch)
{
    if (reason != BranchData::NoBranch) {
        /* Bump up the stream sequence number on a real branch*/
        if (BranchData::isStreamChange(reason))
            executeInfo[tid].streamSeqNum++;

        /* Branches (even mis-predictions) don't change the predictionSeqNum,
         *  just the streamSeqNum */
        branch = BranchData(reason, tid,
            executeInfo[tid].streamSeqNum,
            /* Maintaining predictionSeqNum if there's no inst is just a
             * courtesy and looks better on minorview */
            (inst->isBubble() ? executeInfo[tid].lastPredictionSeqNum
                : inst->id.predictionSeqNum),
            target, inst);

        DPRINTF(Branch, "Branch data signalled: %s\n", branch);
    }
}

void
Execute::handleMemResponse(MinorDynInstPtr inst,
    LSQ::LSQRequestPtr response, BranchData &branch, Fault &fault)
{
    ThreadID thread_id = inst->id.threadId;
    ThreadContext *thread = cpu.getContext(thread_id);

    ExecContext context(cpu, *cpu.threads[thread_id], *this, inst);

    PacketPtr packet = response->packet;

    bool is_load = inst->staticInst->isLoad();
    bool is_store = inst->staticInst->isStore();
    bool is_atomic = inst->staticInst->isAtomic();
    bool is_prefetch = inst->staticInst->isDataPrefetch();

    /* If true, the trace's predicate value will be taken from the exec
     *  context predicate, otherwise, it will be set to false */
    bool use_context_predicate = true;

    if (inst->translationFault != NoFault) {
        /* Invoke memory faults. */
        DPRINTF(MinorMem, "Completing fault from DTLB access: %s\n",
            inst->translationFault->name());

        if (inst->staticInst->isPrefetch()) {
            DPRINTF(MinorMem, "Not taking fault on prefetch: %s\n",
                inst->translationFault->name());

            /* Don't assign to fault */
        } else {
            /* Take the fault raised during the TLB/memory access */
            fault = inst->translationFault;

            fault->invoke(thread, inst->staticInst);
        }
    } else if (!packet) {
        DPRINTF(MinorMem, "Completing failed request inst: %s\n",
            *inst);
        use_context_predicate = false;
        if (!context.readMemAccPredicate())
            inst->staticInst->completeAcc(nullptr, &context, inst->traceData);
    } else if (packet->isError()) {
        DPRINTF(MinorMem, "Trying to commit error response: %s\n",
            *inst);

        fatal("Received error response packet for inst: %s\n", *inst);
    } else if (is_store || is_load || is_prefetch || is_atomic) {
        assert(packet);

        DPRINTF(MinorMem, "Memory response inst: %s addr: 0x%x size: %d\n",
            *inst, packet->getAddr(), packet->getSize());

        if (is_load && packet->getSize() > 0) {
            DPRINTF(MinorMem, "Memory data[0]: 0x%x\n",
                static_cast<unsigned int>(packet->getConstPtr<uint8_t>()[0]));
        }

        /* Complete the memory access instruction */
        fault = inst->staticInst->completeAcc(packet, &context,
            inst->traceData);

        if (fault != NoFault) {
            /* Invoke fault created by instruction completion */
            DPRINTF(MinorMem, "Fault in memory completeAcc: %s\n",
                fault->name());
            fault->invoke(thread, inst->staticInst);
        } else {
            /* Stores need to be pushed into the store buffer to finish
             *  them off */
            if (response->needsToBeSentToStoreBuffer())
                lsq.sendStoreToStoreBuffer(response);
        }
    } else {
        fatal("There should only ever be reads, "
            "writes or faults at this point\n");
    }

    lsq.popResponse(response);

    if (inst->traceData) {
        inst->traceData->setPredicate((use_context_predicate ?
            context.readPredicate() : false));
    }

    doInstCommitAccounting(inst);

    /* Generate output to account for branches */
    tryToBranch(inst, fault, branch);
}

bool
Execute::isInterrupted(ThreadID thread_id) const
{
    return cpu.checkInterrupts(cpu.getContext(thread_id));
}

bool
Execute::takeInterrupt(ThreadID thread_id, BranchData &branch)
{
    DPRINTF(MinorInterrupt, "Considering interrupt status from PC: %s\n",
        cpu.getContext(thread_id)->pcState());

    Fault interrupt = cpu.getInterruptController(thread_id)->getInterrupt
        (cpu.getContext(thread_id));

    if (interrupt != NoFault) {
        /* The interrupt *must* set pcState */
        cpu.getInterruptController(thread_id)->updateIntrInfo
            (cpu.getContext(thread_id));
        interrupt->invoke(cpu.getContext(thread_id));

        assert(!lsq.accessesInFlight());

        DPRINTF(MinorInterrupt, "Invoking interrupt: %s to PC: %s\n",
            interrupt->name(), cpu.getContext(thread_id)->pcState());

        /* Assume that an interrupt *must* cause a branch.  Assert this? */

        updateBranchData(thread_id, BranchData::Interrupt,
            MinorDynInst::bubble(), cpu.getContext(thread_id)->pcState(),
            branch);
    }

    return interrupt != NoFault;
}

bool
Execute::executeMemRefInst(MinorDynInstPtr inst, BranchData &branch,
    bool &passed_predicate, Fault &fault)
{
    bool issued = false;

    /* Set to true if the mem op. is issued and sent to the mem system */
    passed_predicate = false;

    if (!lsq.canRequest()) {
        /* Not acting on instruction yet as the memory
         * queues are full */
        issued = false;
    } else {
        ThreadContext *thread = cpu.getContext(inst->id.threadId);
        TheISA::PCState old_pc = thread->pcState();

        ExecContext context(cpu, *cpu.threads[inst->id.threadId],
            *this, inst);

        DPRINTF(MinorExecute, "Initiating memRef inst: %s\n", *inst);

        Fault init_fault = inst->staticInst->initiateAcc(&context,
            inst->traceData);

        if (inst->inLSQ) {
            if (init_fault != NoFault) {
                assert(inst->translationFault != NoFault);
                // Translation faults are dealt with in handleMemResponse()
                init_fault = NoFault;
            } else {
                // If we have a translation fault then it got suppressed  by
                // initateAcc()
                inst->translationFault = NoFault;
            }
        }

        if (init_fault != NoFault) {
            DPRINTF(MinorExecute, "Fault on memory inst: %s"
                " initiateAcc: %s\n", *inst, init_fault->name());
            fault = init_fault;
        } else {
            /* Only set this if the instruction passed its
             * predicate */
            if (!context.readMemAccPredicate()) {
                DPRINTF(MinorMem, "No memory access for inst: %s\n", *inst);
                assert(context.readPredicate());
            }
            passed_predicate = context.readPredicate();

            /* Set predicate in tracing */
            if (inst->traceData)
                inst->traceData->setPredicate(passed_predicate);

            /* If the instruction didn't pass its predicate
             * or it is a predicated vector instruction and the
             * associated predicate register is all-false (and so will not
             * progress from here)  Try to branch to correct and branch
             * mis-prediction. */
            if (!inst->inLSQ) {
                /* Leave it up to commit to handle the fault */
                lsq.pushFailedRequest(inst);
                inst->inLSQ = true;
            }
        }

        /* Restore thread PC */
        thread->pcState(old_pc);
        issued = true;
    }

    return issued;
}

/** Increment a cyclic buffer index for indices [0, cycle_size-1] */
inline unsigned int
cyclicIndexInc(unsigned int index, unsigned int cycle_size)
{
    unsigned int ret = index + 1;

    if (ret == cycle_size)
        ret = 0;

    return ret;
}

/** Decrement a cyclic buffer index for indices [0, cycle_size-1] */
inline unsigned int
cyclicIndexDec(unsigned int index, unsigned int cycle_size)
{
    int ret = index - 1;

    if (ret < 0)
        ret = cycle_size - 1;

    return ret;
}

unsigned int
Execute::issue(ThreadID thread_id)
{
    const ForwardInstData *insts_in = getInput(thread_id);
    ExecuteThreadInfo &thread = executeInfo[thread_id];

    /* Early termination if we have no instructions */
    if (!insts_in)
        return 0;

    /* Start from the first FU */
    unsigned int fu_index = 0;

    /* Remains true while instructions are still being issued.  If any
     *  instruction fails to issue, this is set to false and we exit issue.
     *  This strictly enforces in-order issue.  For other issue behaviours,
     *  a more complicated test in the outer while loop below is needed. */
    bool issued = true;

    /* Number of insts issues this cycle to check for issueLimit */
    unsigned num_insts_issued = 0;

    /* Number of memory ops issues this cycle to check for memoryIssueLimit */
    unsigned num_mem_insts_issued = 0;

    /* Number of instructions discarded this cycle in order to enforce a
     *  discardLimit. @todo, add that parameter? */
    unsigned num_insts_discarded = 0;

    do {
        MinorDynInstPtr inst = insts_in->insts[thread.inputIndex];
        Fault fault = inst->fault;
        bool discarded = false;
        bool issued_mem_ref = false;

        DPRINTF(MinorExecute, " ............... : %s \n", *inst);


        if (inst->isBubble()) {
            /* Skip */
            issued = true;
        } else if (cpu.getContext(thread_id)->status() ==
            ThreadContext::Suspended)
        {
            DPRINTF(MinorExecute, "Discarding inst: %s from suspended"
                " thread\n", *inst);

            issued = true;
            discarded = true;
        } else if (inst->id.streamSeqNum != thread.streamSeqNum) {
            DPRINTF(MinorExecute, "Discarding inst: %s as its stream"
                " state was unexpected, expected: %d\n",
                *inst, thread.streamSeqNum);
            issued = true;
            discarded = true;
        } else if(!thread.inFlightInsts->canReserve()) {
            DPRINTF(MinorExecute, "Instruction: %s not issued because"
                " there are too many in flight\n", *inst);
            issued = false;
        } else {

            /* Try and issue an instruction into an FU, assume we didn't and
             * fix that in the loop */
            issued = false;

            /* Try FU from 0 each instruction */
            fu_index = 0;

            /* Try and issue a single instruction stepping through the
             *  available FUs */
            do {
                FUPipeline *fu = funcUnits[fu_index];

                DPRINTF(MinorExecute, "Trying to issue inst: %s to FU: %d\n",
                    *inst, fu_index);

                /* Does the examined fu have the OpClass-related capability
                 *  needed to execute this instruction?  Faults can always
                 *  issue to any FU but probably should just 'live' in the
                 *  inFlightInsts queue rather than having an FU. */
                bool fu_is_capable = (!inst->isFault() ?
                    fu->provides(inst->staticInst->opClass()) : true);

                if (inst->isNoCostInst()) {
                        /* Issue free insts. to a fake numbered FU */
                        fu_index = noCostFUIndex;

                        /* And start the countdown on activity to allow
                         *  this instruction to get to the end of its FU */
                        cpu.activityRecorder->activity();

                        /* Mark the destinations for this instruction as
                         *  busy */
                        scoreboard[thread_id].markupInstDests(inst, cpu.curCycle() +
                            Cycles(0), cpu.getContext(thread_id), false);

                        DPRINTF(MinorExecute, "Issuing %s to %d\n",
                            inst->id, noCostFUIndex);
                        inst->fuIndex = noCostFUIndex;
                        inst->extraCommitDelay = Cycles(0);
                        inst->extraCommitDelayExpr = NULL;

                        /* Push the instruction onto the inFlight queue so
                        *  it can be committed in order */
                        QueuedInst fu_inst(inst);
                        thread.inFlightInsts->push(fu_inst);

                        issued = true;
                } else if (!fu_is_capable || fu->alreadyPushed()) {
                    /* Skip */
                    if (!fu_is_capable) {
                        DPRINTF(MinorExecute, "Can't issue as FU: %d isn't"
                            " capable\n", fu_index);
                    } else {
                        DPRINTF(MinorExecute, "Can't issue as FU: %d is"
                            " already busy\n", fu_index);
                    }
                } else if (fu->stalled) {
                    DPRINTF(MinorExecute, "Can't issue inst: %s into FU: %d,"
                        " it's stalled\n",
                        *inst, fu_index);
                } else if (!fu->canInsert()) {
                    DPRINTF(MinorExecute, "Can't issue inst: %s to busy FU"
                        " for another: %d cycles\n",
                        *inst, fu->cyclesBeforeInsert());
                } else {
                    MinorFUTiming *timing = (!inst->isFault() ?
                        fu->findTiming(inst->staticInst) : NULL);

                    const std::vector<Cycles> *src_latencies =
                        (timing ? &(timing->srcRegsRelativeLats)
                            : NULL);

                    const std::vector<bool> *cant_forward_from_fu_indices =
                        &(fu->cantForwardFromFUIndices);

                    if (timing && timing->suppress) {
                        DPRINTF(MinorExecute, "Can't issue inst: %s as extra"
                            " decoding is suppressing it\n",
                            *inst);
                    } else if (!scoreboard[thread_id].canInstIssue(inst,
                        src_latencies, cant_forward_from_fu_indices,
                        cpu.curCycle(), cpu.getContext(thread_id)))
                    {
                        DPRINTF(MinorExecute, "Can't issue inst: %s yet\n",
                            *inst);
                    } else {
                        /* Can insert the instruction into this FU */
                        DPRINTF(MinorExecute, "Issuing inst: %s"
                            " into FU %d\n", *inst,
                            fu_index);

                        Cycles extra_dest_retire_lat = Cycles(0);
                        TimingExpr *extra_dest_retire_lat_expr = NULL;
                        Cycles extra_assumed_lat = Cycles(0);

                        /* Add the extraCommitDelay and extraAssumeLat to
                         *  the FU pipeline timings */
                        if (timing) {
                            extra_dest_retire_lat =
                                timing->extraCommitLat;
                            extra_dest_retire_lat_expr =
                                timing->extraCommitLatExpr;
                            extra_assumed_lat =
                                timing->extraAssumedLat;
                        }

                        issued_mem_ref = inst->isMemRef();

                        QueuedInst fu_inst(inst);

                        /* Decorate the inst with FU details */
                        inst->fuIndex = fu_index;
                        inst->extraCommitDelay = extra_dest_retire_lat;
                        inst->extraCommitDelayExpr =
                            extra_dest_retire_lat_expr;

                        if (issued_mem_ref) {
                            /* Remember which instruction this memory op
                             *  depends on so that initiateAcc can be called
                             *  early */
                            if (allowEarlyMemIssue) {
                                inst->instToWaitFor =
                                    scoreboard[thread_id].execSeqNumToWaitFor(inst,
                                        cpu.getContext(thread_id));

                                if (lsq.getLastMemBarrier(thread_id) >
                                    inst->instToWaitFor)
                                {
                                    DPRINTF(MinorExecute, "A barrier will"
                                        " cause a delay in mem ref issue of"
                                        " inst: %s until after inst"
                                        " %d(exec)\n", *inst,
                                        lsq.getLastMemBarrier(thread_id));

                                    inst->instToWaitFor =
                                        lsq.getLastMemBarrier(thread_id);
                                } else {
                                    DPRINTF(MinorExecute, "Memory ref inst:"
                                        " %s must wait for inst %d(exec)"
                                        " before issuing\n",
                                        *inst, inst->instToWaitFor);
                                }

                                inst->canEarlyIssue = true;
                            }
                            /* Also queue this instruction in the memory ref
                             *  queue to ensure in-order issue to the LSQ */
                            DPRINTF(MinorExecute, "Pushing mem inst: %s\n",
                                *inst);
                            thread.inFUMemInsts->push(fu_inst);
                        }

                        /* Issue to FU */
                        fu->push(fu_inst);
                        /* And start the countdown on activity to allow
                         *  this instruction to get to the end of its FU */
                        cpu.activityRecorder->activity();

                        /* Mark the destinations for this instruction as
                         *  busy */
                        scoreboard[thread_id].markupInstDests(inst, cpu.curCycle() +
                            fu->description.opLat +
                            extra_dest_retire_lat +
                            extra_assumed_lat,
                            cpu.getContext(thread_id),
                            issued_mem_ref && extra_assumed_lat == Cycles(0));

                        /* Push the instruction onto the inFlight queue so
                         *  it can be committed in order */
                        thread.inFlightInsts->push(fu_inst);

                        issued = true;
                    }
                }

                fu_index++;
            } while (fu_index != numFuncUnits && !issued);

            if (!issued)
                DPRINTF(MinorExecute, "Didn't issue inst: %s\n", *inst);
        }

        if (issued) {
            if(inst->isInst() && inst->staticInst->isSSRecv()) {
                ssim.forward_progress(cpu.curCycle());
            }

            /* Generate MinorTrace's MinorInst lines.  Do this at commit
             *  to allow better instruction annotation? */
            if (DTRACE(MinorTrace) && !inst->isBubble())
                inst->minorTraceInst(*this);

            /* Mark up barriers in the LSQ */
            if (!discarded && inst->isInst() &&
                inst->staticInst->isMemBarrier())
            {
                DPRINTF(MinorMem, "Issuing memory barrier inst: %s\n", *inst);
                lsq.issuedMemBarrierInst(inst);
            }

            if (inst->traceData && setTraceTimeOnIssue) {
                inst->traceData->setWhen(curTick());
            }

            if (issued_mem_ref)
                num_mem_insts_issued++;

            if (discarded) {
                num_insts_discarded++;
                ssim.issued_discarded_inst();
            } else if (!inst->isBubble()) {
                num_insts_issued++;
                ssim.issued_inst();
                if (num_insts_issued == issueLimit)
                    DPRINTF(MinorExecute, "Reached inst issue limit\n");
            }
            ssim.statistics.countHostInst(inst->staticInst, discarded);

            thread.inputIndex++;
            DPRINTF(MinorExecute, "Stepping to next inst inputIndex: %d\n",
                thread.inputIndex);
        }

        /* Got to the end of a line */
        if (thread.inputIndex == insts_in->width()) {
            popInput(thread_id);
            /* Set insts_in to null to force us to leave the surrounding
             *  loop */
            insts_in = NULL;

            if (processMoreThanOneInput) {
                DPRINTF(MinorExecute, "Wrapping\n");
                insts_in = getInput(thread_id);
            }
        }
    } while (insts_in && thread.inputIndex < insts_in->width() &&
        /* We still have instructions */
        fu_index != numFuncUnits && /* Not visited all FUs */
        issued && /* We've not yet failed to issue an instruction */
        num_insts_issued != issueLimit && /* Still allowed to issue */
        num_mem_insts_issued != memoryIssueLimit);

    return num_insts_issued;
}

bool
Execute::tryPCEvents(ThreadID thread_id)
{
    ThreadContext *thread = cpu.getContext(thread_id);
    unsigned int num_pc_event_checks = 0;

    /* Handle PC events on instructions */
    Addr oldPC;
    do {
        oldPC = thread->instAddr();
        cpu.threads[thread_id]->pcEventQueue.service(oldPC, thread);
        num_pc_event_checks++;
    } while (oldPC != thread->instAddr());

    if (num_pc_event_checks > 1) {
        DPRINTF(PCEvent, "Acting on PC Event to PC: %s\n",
            thread->pcState());
    }

    return num_pc_event_checks > 1;
}

void
Execute::doInstCommitAccounting(MinorDynInstPtr inst)
{
    assert(!inst->isFault());

    MinorThread *thread = cpu.threads[inst->id.threadId];

    /* Increment the many and various inst and op counts in the
     *  thread and system */
    if (!inst->staticInst->isMicroop() || inst->staticInst->isLastMicroop())
    {
        thread->numInst++;
        thread->numInsts++;
        cpu.stats.numInsts++;
        cpu.system->totalNumInsts++;

        /* Act on events related to instruction counts */
        thread->comInstEventQueue.serviceEvents(thread->numInst);
    }
    thread->numOp++;
    thread->numOps++;
    cpu.stats.numOps++;
    cpu.stats.committedInstType[inst->id.threadId]
                               [inst->staticInst->opClass()]++;

    /* Set the CP SeqNum to the numOps commit number */
    if (inst->traceData)
        inst->traceData->setCPSeq(thread->numOp);

    cpu.probeInstCommit(inst->staticInst, inst->pc.instAddr());
}

void Execute::timeout_check(bool should_commit, MinorDynInstPtr inst) {
  uint64_t cyc = cpu.curCycle();
  uint64_t last_event = std::max(last_sd_issue, ssim.forward_progress_cycle() / cpu.frequency());

  if(!should_commit) {
    DSA_LOG(TICK) << "Not commit: " << ssim.forward_progress_cycle() / cpu.frequency()
      << ", " << cpu.curCycle();
    if (cyc > 100000 + last_event) {
      ssim.print_stats();
      DSA_CHECK(false) << "Instruction: " << *inst << " is stalled for too long!!! ABORTING";
      exit(1);
    }
  } else {
    last_sd_issue = cyc;
  }
}


bool
Execute::commitInst(MinorDynInstPtr inst, bool early_memory_issue,
    BranchData &branch, Fault &fault, bool &committed,
    bool &completed_mem_issue)
{
    ThreadID thread_id = inst->id.threadId;
    ThreadContext *thread = cpu.getContext(thread_id);

    bool completed_inst = true;
    fault = NoFault;

    /* Is the thread for this instruction suspended?  In that case, just
     *  stall as long as there are no pending interrupts */
    if (thread->status() == ThreadContext::Suspended &&
        !isInterrupted(thread_id))
    {
        panic("We should never hit the case where we try to commit from a "
              "suspended thread as the streamSeqNum should not match");
    } else if (inst->isFault()) {
        ExecContext context(cpu, *cpu.threads[thread_id], *this, inst);

        DPRINTF(MinorExecute, "Fault inst reached Execute: %s\n",
            inst->fault->name());

        fault = inst->fault;
        inst->fault->invoke(thread, NULL);

        tryToBranch(inst, fault, branch);
    } else if (inst->staticInst->isMemRef()) {
        /* Memory accesses are executed in two parts:
         *  executeMemRefInst -- calculates the EA and issues the access
         *      to memory.  This is done here.
         *  handleMemResponse -- handles the response packet, done by
         *      Execute::commit
         *
         *  While the memory access is in its FU, the EA is being
         *  calculated.  At the end of the FU, when it is ready to
         *  'commit' (in this function), the access is presented to the
         *  memory queues.  When a response comes back from memory,
         *  Execute::commit will commit it.
         */
        bool predicate_passed = false;
        bool completed_mem_inst = executeMemRefInst(inst, branch,
            predicate_passed, fault);

        /*if(cpu.cpuId()==1 && inst->staticInst->isStore()) {
          std::cout << "Completed mem inst?: " << completed_mem_inst << "\n"; 
          std::cout << "Could lsq request? " << lsq.canRequest() << "\n";
          std::cout << "Num of access issued to mem? " << lsq.accessesInFlight() << "\n";
        }*/

        if (completed_mem_inst && fault != NoFault) {
            if (early_memory_issue) {
                DPRINTF(MinorExecute, "Fault in early executing inst: %s\n",
                    fault->name());
                /* Don't execute the fault, just stall the instruction
                 *  until it gets to the head of inFlightInsts */
                inst->canEarlyIssue = false;
                /* Not completed as we'll come here again to pick up
                 * the fault when we get to the end of the FU */
                completed_inst = false;
            } else {
                DPRINTF(MinorExecute, "Fault in execute: %s\n",
                    fault->name());
                fault->invoke(thread, NULL);

                tryToBranch(inst, fault, branch);
                completed_inst = true;
            }
        } else {
            completed_inst = completed_mem_inst;
        }
        completed_mem_issue = completed_inst;
    } else if (inst->isInst() && inst->staticInst->isMemBarrier() &&
        !lsq.canPushIntoStoreBuffer())
    {
        DPRINTF(MinorExecute, "Can't commit data barrier inst: %s yet as"
            " there isn't space in the store buffer\n", *inst);

        completed_inst = false;
    } else if (inst->isInst() && inst->staticInst->isQuiesce()
            && !branch.isBubble()){
        /* This instruction can suspend, need to be able to communicate
         * backwards, so no other branches may evaluate this cycle*/
        completed_inst = false;
    } else if (inst->staticInst->isSSRecv() &&
               !ssim.CanReceive(inst->staticInst->get_imm())) {
        /* Don't commit if you can't receive on output port*/
        DPRINTF(SS, "Could Not Recv: %s\n", *inst);
        completed_inst = false;
        timeout_check(false, inst);
    } else if (inst->staticInst->isSS() && ssim.is_in_config()) {
        completed_inst = false;
        ssim.wait_config();
        timeout_check(false, inst);
        /* Don't execute any instructions if ssim is in config mode!*/
    } else if (inst->staticInst->isSSConfig() && !lsq.canRequest()) {
        completed_inst = false;
        ssim.wait_config();
        timeout_check(false, inst);
    } else {
        bool should_commit = true;

        // for spu global barrier
        // ExecContext context(cpu, *cpu.threads[0], *this, inst);

        //break down by type
        bool stallBySSWait = false;
        int waitMask = 0;
        if (inst->staticInst->isSSWait()) {
          auto &regIdx = inst->staticInst->srcRegIdx(0);
          DSA_CHECK(regIdx.isIntReg());
          waitMask = thread->readIntReg(regIdx.index());
          stallBySSWait = ssim_t::stall_core(waitMask);
        }

        if ((inst->staticInst->isSSStream() || stallBySSWait) && !ssim.StreamBufferAvailable()) {
          should_commit = false;
          DPRINTF(SS,"Can't issue stream b/c buffer is full");
          // continue;
        } else if (stallBySSWait) {
          if (!ssim.done(false, waitMask)) {
            should_commit = false;
            ssim.wait_inst(waitMask); //track stats
            DPRINTF(SS,"Wait blocked, mask: %x\n", waitMask);
          } else {
            DSA_LOG(COMMAND)
              << curTick() << ": Wait complete, mask: "
              << std::bitset<32>(waitMask).to_string();
          }
        }

        if(inst->staticInst->isSS()) {
          timeout_check(should_commit, inst);
        }

        if(should_commit) {
          ExecContext context(cpu, *cpu.threads[thread_id], *this, inst);

          DPRINTF(MinorExecute, "Committing inst: %s\n", *inst);

          fault = inst->staticInst->execute(&context, inst->traceData);

          /* Set the predicate for tracing and dump */
          if (inst->traceData)
              inst->traceData->setPredicate(context.readPredicate());

          committed = true;

          if (fault != NoFault) {
              DPRINTF(MinorExecute, "Fault in execute of inst: %s fault: %s\n",
                  *inst, fault->name());
              fault->invoke(thread, inst->staticInst);
          }

          doInstCommitAccounting(inst);
          tryToBranch(inst, fault, branch);
        } else {
          completed_inst=false;
        }
    }

    if (completed_inst) {
        /* Keep a copy of this instruction's predictionSeqNum just in case
         * we need to issue a branch without an instruction (such as an
         * interrupt) */
        executeInfo[thread_id].lastPredictionSeqNum = inst->id.predictionSeqNum;

        /* Check to see if this instruction suspended the current thread. */
        if (!inst->isFault() &&
            thread->status() == ThreadContext::Suspended &&
            branch.isBubble() && /* It didn't branch too */
            !isInterrupted(thread_id)) /* Don't suspend if we have
                interrupts */
        {
            TheISA::PCState resume_pc = cpu.getContext(thread_id)->pcState();

            assert(resume_pc.microPC() == 0);

            DPRINTF(MinorInterrupt, "Suspending thread: %d from Execute"
                " inst: %s\n", thread_id, *inst);

            cpu.stats.numFetchSuspends++;

            updateBranchData(thread_id, BranchData::SuspendThread, inst,
                resume_pc, branch);
        }
    }

    return completed_inst;
}

void
Execute::commit(ThreadID thread_id, bool only_commit_microops, bool discard,
    BranchData &branch)
{
    Fault fault = NoFault;
    Cycles now = cpu.curCycle();
    ExecuteThreadInfo &ex_info = executeInfo[thread_id];

    /**
     * Try and execute as many instructions from the end of FU pipelines as
     *  possible.  This *doesn't* include actually advancing the pipelines.
     *
     * We do this by looping on the front of the inFlightInsts queue for as
     *  long as we can find the desired instruction at the end of the
     *  functional unit it was issued to without seeing a branch or a fault.
     *  In this function, these terms are used:
     *      complete -- The instruction has finished its passage through
     *          its functional unit and its fate has been decided
     *          (committed, discarded, issued to the memory system)
     *      commit -- The instruction is complete(d), not discarded and has
     *          its effects applied to the CPU state
     *      discard(ed) -- The instruction is complete but not committed
     *          as its streamSeqNum disagrees with the current
     *          Execute::streamSeqNum
     *
     *  Commits are also possible from two other places:
     *
     *  1) Responses returning from the LSQ
     *  2) Mem ops issued to the LSQ ('committed' from the FUs) earlier
     *      than their position in the inFlightInsts queue, but after all
     *      their dependencies are resolved.
     */

    /* Has an instruction been completed?  Once this becomes false, we stop
     *  trying to complete instructions. */
    bool completed_inst = true;

    /* Number of insts committed this cycle to check against commitLimit */
    unsigned int num_insts_committed = 0;

    /* Number of memory access instructions committed to check against
     *  memCommitLimit */
    unsigned int num_mem_refs_committed = 0;

    if (only_commit_microops && !ex_info.inFlightInsts->empty()) {
        DPRINTF(MinorInterrupt, "Only commit microops %s %d\n",
            *(ex_info.inFlightInsts->front().inst),
            ex_info.lastCommitWasEndOfMacroop);
    }

    while (!ex_info.inFlightInsts->empty() && /* Some more instructions to process */
        !branch.isStreamChange() && /* No real branch */
        fault == NoFault && /* No faults */
        completed_inst && /* Still finding instructions to execute */
        // num_insts_committed != commitLimit /* Not reached commit limit */
        num_insts_committed != commitLimit && /* Not reached commit limit */
        cpu.getContext(thread_id)->status() != ThreadContext::Suspended
        )
    {
        if (only_commit_microops) {
            DPRINTF(MinorInterrupt, "Committing tail of insts before"
                " interrupt: %s\n",
                *(ex_info.inFlightInsts->front().inst));
        }

        QueuedInst *head_inflight_inst = &(ex_info.inFlightInsts->front());

        InstSeqNum head_exec_seq_num =
            head_inflight_inst->inst->id.execSeqNum;

        /* The instruction we actually process if completed_inst
         *  remains true to the end of the loop body.
         *  Start by considering the the head of the in flight insts queue */
        MinorDynInstPtr inst = head_inflight_inst->inst;

        bool committed_inst = false;
        bool discard_inst = false;
        bool completed_mem_ref = false;
        bool issued_mem_ref = false;
        bool early_memory_issue = false;

        /* Must set this again to go around the loop */
        completed_inst = false;

        /* If we're just completing a macroop before an interrupt or drain,
         *  can we stil commit another microop (rather than a memory response)
         *  without crosing into the next full instruction? */
        bool can_commit_insts = !ex_info.inFlightInsts->empty() &&
            !(only_commit_microops && ex_info.lastCommitWasEndOfMacroop);

        /* Can we find a mem response for this inst */
        LSQ::LSQRequestPtr mem_response =
            (inst->inLSQ ? lsq.findResponse(inst) : NULL);

        DPRINTF(MinorExecute, "Trying to commit canCommitInsts: %d\n",
            can_commit_insts);

        /* Test for PC events after every instruction */
        if (isInbetweenInsts(thread_id) && tryPCEvents(thread_id)) {
            ThreadContext *thread = cpu.getContext(thread_id);

            /* Branch as there was a change in PC */
            updateBranchData(thread_id, BranchData::UnpredictedBranch,
                MinorDynInst::bubble(), thread->pcState(), branch);
        } else if (mem_response &&
            num_mem_refs_committed < memoryCommitLimit)
        {
            /* Try to commit from the memory responses next */
            discard_inst = inst->id.streamSeqNum !=
                           ex_info.streamSeqNum || discard;

            DPRINTF(MinorExecute, "inst: %s seq number: %d"
                    " , expected: %d\n",
                    *inst, inst->id.streamSeqNum, ex_info.streamSeqNum);

            DPRINTF(MinorExecute, "Trying to commit mem response: %s\n",
                *inst);

            /* Complete or discard the response */
            if (discard_inst) {
                DPRINTF(MinorExecute, "Discarding mem inst: %s as its"
                    " stream state was unexpected, expected: %d\n",
                    *inst, ex_info.streamSeqNum);

                lsq.popResponse(mem_response);
            } else {
                handleMemResponse(inst, mem_response, branch, fault);
                committed_inst = true;
            }

            completed_mem_ref = true;
            completed_inst = true;
        } else if (can_commit_insts) {
            /* If true, this instruction will, subject to timing tweaks,
             *  be considered for completion.  try_to_commit flattens
             *  the `if' tree a bit and allows other tests for inst
             *  commit to be inserted here. */
            bool try_to_commit = false;

            /* Try and issue memory ops early if they:
             *  - Can push a request into the LSQ
             *  - Have reached the end of their FUs
             *  - Have had all their dependencies satisfied
             *  - Are from the right stream
             *
             *  For any other case, leave it to the normal instruction
             *  issue below to handle them.
             */
            if (!ex_info.inFUMemInsts->empty() && lsq.canRequest()) {
                DPRINTF(MinorExecute, "Trying to commit from mem FUs\n");

                const MinorDynInstPtr head_mem_ref_inst =
                    ex_info.inFUMemInsts->front().inst;
                FUPipeline *fu = funcUnits[head_mem_ref_inst->fuIndex];
                const MinorDynInstPtr &fu_inst = fu->front().inst;

                /* Use this, possibly out of order, inst as the one
                 *  to 'commit'/send to the LSQ */
                if (!fu_inst->isBubble() &&
                    !fu_inst->inLSQ &&
                    fu_inst->canEarlyIssue &&
                    ex_info.streamSeqNum == fu_inst->id.streamSeqNum &&
                    head_exec_seq_num > fu_inst->instToWaitFor)
                {
                    DPRINTF(MinorExecute, "Issuing mem ref early"
                        " inst: %s instToWaitFor: %d\n",
                        *(fu_inst), fu_inst->instToWaitFor);

                    inst = fu_inst;
                    try_to_commit = true;
                    early_memory_issue = true;
                    completed_inst = true;
                }
            }

            /* Try and commit FU-less insts */
            if (!completed_inst && inst->isNoCostInst()) {
                DPRINTF(MinorExecute, "Committing no cost inst: %s\n", *inst);

                try_to_commit = true;
                completed_inst = true;
            }

            /* Try to issue from the ends of FUs and the inFlightInsts
             *  queue */
            if (!completed_inst && !inst->inLSQ) {
                DPRINTF(MinorExecute, "Trying to commit from FUs\n");

                /* Try to commit from a functional unit */
                /* Is the head inst of the expected inst's FU actually the
                 *  expected inst? */
                QueuedInst &fu_inst =
                    funcUnits[inst->fuIndex]->front();
                InstSeqNum fu_inst_seq_num = fu_inst.inst->id.execSeqNum;

                if (fu_inst.inst->isBubble()) {
                    /* No instruction ready */
                    completed_inst = false;
                } else if (fu_inst_seq_num != head_exec_seq_num) {
                    /* Past instruction: we must have already executed it
                     * in the same cycle and so the head inst isn't
                     * actually at the end of its pipeline
                     * Future instruction: handled above and only for
                     * mem refs on their way to the LSQ */
                } else if (fu_inst.inst->id == inst->id)  {
                    /* All instructions can be committed if they have the
                     *  right execSeqNum and there are no in-flight
                     *  mem insts before us */
                    try_to_commit = true;
                    completed_inst = true;
                }
            }

            if (try_to_commit) {
                discard_inst = inst->id.streamSeqNum !=
                    ex_info.streamSeqNum || discard;

                /* Is this instruction discardable as its streamSeqNum
                 *  doesn't match? */
                if (!discard_inst) {
                    /* Try to commit or discard a non-memory instruction.
                     *  Memory ops are actually 'committed' from this FUs
                     *  and 'issued' into the memory system so we need to
                     *  account for them later (commit_was_mem_issue gets
                     *  set) */
                    if (inst->extraCommitDelayExpr) {
                        DPRINTF(MinorExecute, "Evaluating expression for"
                            " extra commit delay inst: %s\n", *inst);

                        ThreadContext *thread = cpu.getContext(thread_id);

                        TimingExprEvalContext context(inst->staticInst,
                            thread, NULL);

                        uint64_t extra_delay = inst->extraCommitDelayExpr->
                            eval(context);

                        DPRINTF(MinorExecute, "Extra commit delay expr"
                            " result: %d\n", extra_delay);

                        if (extra_delay < 128) {
                            inst->extraCommitDelay += Cycles(extra_delay);
                        } else {
                            DPRINTF(MinorExecute, "Extra commit delay was"
                                " very long: %d\n", extra_delay);
                        }
                        inst->extraCommitDelayExpr = NULL;
                    }

                    /* Move the extraCommitDelay from the instruction
                     *  into the minimumCommitCycle */
                    if (inst->extraCommitDelay != Cycles(0)) {
                        inst->minimumCommitCycle = cpu.curCycle() +
                            inst->extraCommitDelay;
                        inst->extraCommitDelay = Cycles(0);
                    }

                    /* @todo Think about making lastMemBarrier be
                     *  MAX_UINT_64 to avoid using 0 as a marker value */
                    if (!inst->isFault() && inst->isMemRef() &&
                        lsq.getLastMemBarrier(thread_id) <
                            inst->id.execSeqNum &&
                        lsq.getLastMemBarrier(thread_id) != 0)
                    {
                        DPRINTF(MinorExecute, "Not committing inst: %s yet"
                            " as there are incomplete barriers in flight\n",
                            *inst);
                        completed_inst = false;
                    } else if (inst->minimumCommitCycle > now) {
                        DPRINTF(MinorExecute, "Not committing inst: %s yet"
                            " as it wants to be stalled for %d more cycles\n",
                            *inst, inst->minimumCommitCycle - now);
                        completed_inst = false;
                    } else {
                        completed_inst = commitInst(inst,
                            early_memory_issue, branch, fault,
                            committed_inst, issued_mem_ref);
                    }
                } else {
                    /* Discard instruction */
                    completed_inst = true;
                }

                if (completed_inst) {
                    /* Allow the pipeline to advance.  If the FU head
                     *  instruction wasn't the inFlightInsts head
                     *  but had already been committed, it would have
                     *  unstalled the pipeline before here */
                    if (inst->fuIndex != noCostFUIndex) {
                        DPRINTF(MinorExecute, "Unstalling %d for inst %s\n", inst->fuIndex, inst->id);
                        funcUnits[inst->fuIndex]->stalled = false;
                    }
                }
            }
        } else {
            DPRINTF(MinorExecute, "No instructions to commit\n");
            completed_inst = false;
        }

        /* All discardable instructions must also be 'completed' by now */
        assert(!(discard_inst && !completed_inst));

        /* Instruction committed but was discarded due to streamSeqNum
         *  mismatch */
        if (discard_inst) {
            DPRINTF(MinorExecute, "Discarding inst: %s as its stream"
                " state was unexpected, expected: %d\n",
                *inst, ex_info.streamSeqNum);

            if (fault == NoFault)
                cpu.stats.numDiscardedOps++;
        }

        /* Mark the mem inst as being in the LSQ */
        if (issued_mem_ref) {
            inst->fuIndex = 0;
            inst->inLSQ = true;
        }

        /* Pop issued (to LSQ) and discarded mem refs from the inFUMemInsts
         *  as they've *definitely* exited the FUs */
        if (completed_inst && inst->isMemRef()) {
            /* The MemRef could have been discarded from the FU or the memory
             *  queue, so just check an FU instruction */
            if (!ex_info.inFUMemInsts->empty() &&
                ex_info.inFUMemInsts->front().inst == inst)
            {
                ex_info.inFUMemInsts->pop();
            }
        }

        if (completed_inst && !(issued_mem_ref && fault == NoFault)) {
            /* Note that this includes discarded insts */
            DPRINTF(MinorExecute, "Completed inst: %s\n", *inst);

            /* Got to the end of a full instruction? */
            ex_info.lastCommitWasEndOfMacroop = inst->isFault() ||
                inst->isLastOpInInst();

            /* lastPredictionSeqNum is kept as a convenience to prevent its
             *  value from changing too much on the minorview display */
            ex_info.lastPredictionSeqNum = inst->id.predictionSeqNum;

            /* Finished with the inst, remove it from the inst queue and
             *  clear its dependencies */
            ex_info.inFlightInsts->pop();

            /* Complete barriers in the LSQ/move to store buffer */
            if (inst->isInst() && inst->staticInst->isMemBarrier()) {
                DPRINTF(MinorMem, "Completing memory barrier"
                    " inst: %s committed: %d\n", *inst, committed_inst);
                lsq.completeMemBarrierInst(inst, committed_inst);
            }

            scoreboard[thread_id].clearInstDests(inst, inst->isMemRef());
        }

        /* Handle per-cycle instruction counting */
        if (committed_inst) {
            bool is_no_cost_inst = inst->isNoCostInst();

            /* Don't show no cost instructions as having taken a commit
             *  slot */
            if (DTRACE(MinorTrace) && !is_no_cost_inst)
                ex_info.instsBeingCommitted.insts[num_insts_committed] = inst;

            if (!is_no_cost_inst)
                num_insts_committed++;

            if (num_insts_committed == commitLimit)
                DPRINTF(MinorExecute, "Reached inst commit limit\n");

            /* Re-set the time of the instruction if that's required for
             * tracing */
            if (inst->traceData) {
                if (setTraceTimeOnCommit)
                    inst->traceData->setWhen(curTick());
                inst->traceData->dump();
            }

            if (completed_mem_ref)
                num_mem_refs_committed++;

            if (num_mem_refs_committed == memoryCommitLimit)
                DPRINTF(MinorExecute, "Reached mem ref commit limit\n");
        }
    }
}

bool
Execute::isInbetweenInsts(ThreadID thread_id) const
{
    return executeInfo[thread_id].lastCommitWasEndOfMacroop &&
        !lsq.accessesInFlight();
}

bool
Execute::check_network_idle() {
  return cpu.check_network_idle();
}

// TODO: see how to set custom messages
// pack the message to send request on the SPU network to write on remote scratchpad
// void Execute::send_spu_scr_wr_req(bool scr_type, int64_t val, int64_t scr_offset, int dest_core_id) {
void Execute::send_spu_scr_wr_req(int8_t* val, int num_bytes, uint64_t scr_offset, int dest_core_id) {

  int req_type=1;
  uint64_t addr_to_send;
  int mcast_dest[1];

  addr_to_send = scr_offset | num_bytes << 16; // TODO: encode data_width
  dest_core_id += 1;
  DSA_LOG(NET_REQ) << "output destination core: " << dest_core_id;
  mcast_dest[0] = dest_core_id;
  spu_req_info req(1, val, num_bytes, addr_to_send, mcast_dest, req_type);
  push_net_req(req);
}

// data from current core to the requesting core
void Execute::push_rem_read_return(int dst_core, int8_t data[64], int request_ptr, int addr, int data_bytes, int reorder_entry) {
  int req_type=0;
  int orig_data_bytes = data_bytes;
  uint32_t addr_to_send;
  int mcast_dest[1];

  if(data_bytes>8) {
    assert(data_bytes==NUM_SCRATCH_BANKS);
    data_bytes=9; // linear case
  }

  assert(addr < (64*1024));
  assert(request_ptr < 64); // this would be equal to log(irob entries)
  assert(data_bytes < 16); // could be 1 to 8 bytes
  // global scratch size < 64 kB
  addr_to_send = addr | request_ptr << 16 | data_bytes << 22 | reorder_entry << 26;
  
  int src_core = cpu.cpuId();
  assert(src_core!=dst_core && "Same core should not have been remote");

  DSA_LOG(NET_REQ) << "Remote read return data src core: " << src_core << " dest core: " << dst_core;
  DSA_LOG(NET_REQ)
      << "Returning remote read data addr: " << addr << " and x dim: "
      << request_ptr << " y dim: " << reorder_entry << " and original data bytes: " << orig_data_bytes;
  mcast_dest[0] = dst_core;

  spu_req_info req(1, data, orig_data_bytes, addr_to_send, mcast_dest, req_type);
  push_net_req(req);
}


bool Execute::push_rem_read_req(int dest_core_id, int request_ptr, int addr, int data_bytes, int reorder_entry) {
  // TODO: to coalesce, can I accumulate this info somewhere until
  // if local request, push back to the core
  
  // int dest_core_id = addr & 1024; // FIXME: should be number of threads
  // int dest_core_id = addr % ssim.num_active_threads();
  // int dest_core_id = addr/SCRATCH_SIZE;
  
  assert(dest_core_id < ssim.num_active_threads());
  dest_core_id += 1;

  int req_core = cpu.cpuId();

  // std::cout << "Address for indirect read: " << addr << "\n";
  // std::cout << "Came in for addr: " << addr << " req_core: " << req_core << " dest core: " << dest_core_id << "\n";

  addr = addr & (SCRATCH_SIZE-1);

  if((ssim.num_active_threads()==1) || (dest_core_id==req_core)) { // 0--host core when non-multi-threaded code
    DSA_LOG(NET_REQ) << "LOCAL REQUEST destination core: " << dest_core_id;
    ssim.push_ind_rem_read_req(false, req_core, request_ptr, addr, data_bytes, reorder_entry);
    return false;
  }

  int req_type=0;

  uint32_t addr_to_send;
  int mcast_dest[1];

  if(data_bytes>8) {
    // assert(data_bytes==NUM_SCRATCH_BANKS);
    assert(data_bytes%NUM_SCRATCH_BANKS==0);
    data_bytes=8+data_bytes/NUM_SCRATCH_BANKS; // 9; // linear case
    assert(data_bytes<=15 && "exceeded the maximum multiple of cache line");
  }

  // TODO: could be more, move data bytes to the data arrat
  assert(addr < SCRATCH_SIZE);
  assert(request_ptr < NUM_SCRATCH_BANKS); // this would be equal to log(irob entries)
  assert(data_bytes<16);

  // TODO: could be more, move data bytes to the data arrat
  addr_to_send = addr | request_ptr << 16 | data_bytes << 22 | reorder_entry << 26; // | req_core << 29;
  DSA_LOG(NET_REQ)
    << "output destination core for scratchpad read: " << dest_core_id
    << " and requesting core: " << cpu.cpuId();
  DSA_LOG(NET_REQ)
    << "Remote read request, scr_addr: " << addr << " and local core(0-indexed): "
    << cpu.cpuId() <<" and addr to send: " << addr_to_send
    << " request ptr: " << request_ptr << " reorder entry: " << reorder_entry; 
  mcast_dest[0] = dest_core_id;

  int8_t val[9];
  val[0]=-1;
  for(int j=0; j<8; ++j){
    val[j+1] = (req_core >> (j*8)) & 65535;
  }
  spu_req_info req(1, val, 9, addr_to_send, mcast_dest, req_type);
  push_net_req(req);
  
  return true;
}

void Execute::push_net_req(spu_req_info req) {
  int j=0;
  int split_count=0;
  while(req.num_data_bytes!=0) {
    if(req.num_dest==0) return;
    assert(req.mcast_dest[0]<=ssim.num_active_threads());
    split_count++;
    std::shared_ptr<SpuRequestMsg> msg = std::make_shared<SpuRequestMsg>(cpu.clockEdge());
    (*msg).m_MessageSize = MessageSizeType_Control;
    (*msg).m_Requestor = cpu.get_m_version();
    switch(req.type) {
      case 0: (*msg).m_Type = SpuRequestType_LD;
              break;
      case 1: (*msg).m_Type = SpuRequestType_ST;
              break;
      case 2: (*msg).m_Type = SpuRequestType_UPDATE;
              break;
      default: assert(0);
    }
    (*msg).m_addr = req.addr_to_send;
    int bytes_to_send = std::min(SPU_NET_PACKET_SIZE, req.num_data_bytes);
    for(j=0; j<bytes_to_send; ++j){ // non-zero if data-request
      (*msg).m_DataBlk.setByte(j,req.data[j]);
    }
    // delimeter used in atomic update requests
    if(j!=SPU_NET_PACKET_SIZE) (*msg).m_DataBlk.setByte(j,-1);

    // std::cout << "Number of dest: " << req.num_dest << "\n";
    for(j=0; j<req.num_dest; ++j){ // non-zero if data-request
      // std::cout << "mast dest: " << req.mcast_dest[j] << "\n";
      (*msg).m_Destination.add(cpu.get_m_version(req.mcast_dest[j]));
    }
    _pending_net_req.push(std::make_pair(msg,req.num_dest)); // ordering may be changed
    req.num_data_bytes -= bytes_to_send;
    req.data += bytes_to_send;
  }
  // std::cout << "Split count this cycle: " << split_count << "\n";
}

// TODO: this should be called at each network cycle not spu?
void Execute::serve_pending_net_req() {
  // requires some condition if this can be accepted or not..
  // check if a control or data message
  if(!_pending_net_req.empty()) {

    DSA_LOG(NET_REQ) << "Issuing SPU network request from core: " << cpu.cpuId() << " at cycle: " << cpu.curCycle();

    ThreadContext *thread = cpu.getContext(0); // assume tid=0?
    for(int i=0; i<_pending_net_req.front().second; ++i) {
      thread->getSystemPtr()->inc_spu_sent();
    }
    // somehow we modified enqueue time
    cpu.pushReqFromSpu(_pending_net_req.front().first);
    _pending_net_req.pop();
  }
}


// TODO: when num_vals is not large (tagging not required), mix in one packet only
// TODO: also adapt it to different datatypes of values (lets keep addresses of
// fixed data-type)
bool Execute::push_rem_atom_op_req(uint64_t val, std::vector<int> update_broadcast_dest, std::vector<int> update_coalesce_vals, int opcode, int val_bytes, int out_bytes) {

  // std::cout << "Received an atomic op request with dest size: " << update_broadcast_dest.size() << " and coalesce size: " << update_coalesce_vals.size() << "\n";
  

  // if(ssim.num_active_threads()<0) assert(0);

  int num_updates = update_broadcast_dest.size();
  int num_vals = update_coalesce_vals.size();


  DSA_LOG(NET_REQ) << "number of broacast: " << num_updates;
  for(unsigned k=0; k<update_broadcast_dest.size(); ++k) {
    DSA_LOG(NET_REQ) << "broadcast addr: " << update_broadcast_dest[k];
  }

  int req_type = 2;
  int values_to_send =  num_vals*val_bytes; // 2*8>
  int req_core = cpu.cpuId();
  std::vector<bool> dest_done;
  dest_done.resize(ssim.num_active_threads(), 0);

  int addr_bytes=4; // FIXME: can be 2 to improve multicast, but currently lets keep it a power-of-2

  unsigned d=0, j=0;
  int tag=-1;
  // Step1: create the tagged packet
  _last_tag = (_last_tag+1)%1024;
  tag = req_core << 10 | _last_tag; // 10 bits of ID and 6 bits of core_id
  std::vector<int> local_dest_scratch_addr;
  int num_active_threads = ssim.num_active_threads();
  int multicast_dest[num_active_threads];
  uint64_t addr_to_send;
  std::vector<int> dest_scratch_addr;
  int num_dest=0;

  // TODO: only the last round will be sent with a tag
  // for others, it will record the start addr until a tagged packet is
  // received 
  for(d=0; d<num_updates; ++d) { 
    int local_scr_addr = update_broadcast_dest[d] & (SCRATCH_SIZE-1);
    int dest_core_id = update_broadcast_dest[d]/SCRATCH_SIZE;
    dest_core_id += 1; // this should be 1 to 8

    // std::cout << "input: " << update_broadcast_dest[d] << "scr addr: " << local_scr_addr << " dest core id: " << dest_core_id << "\n";
    assert(dest_core_id<=num_active_threads);

    if((num_active_threads==1 || dest_core_id==cpu.cpuId())) { // 0--host core when non-multi-threaded code
      if(SS_DEBUG::NET_REQ){
        printf("LOCAL REQUEST destination core: %d\n",dest_core_id);
      }
      local_dest_scratch_addr.push_back(local_scr_addr);
      continue;
    } 
    if(!dest_done[dest_core_id-1]) {
      multicast_dest[num_dest] = dest_core_id;
      num_dest++;
    }
    dest_done[dest_core_id-1]=true;
    dest_scratch_addr.push_back(update_broadcast_dest[d]);
  }
  if(num_dest!=0) {
    // tagged | tag packet | tag...
    // destinations)
    unsigned num_addr_to_send =  dest_scratch_addr.size()*addr_bytes;
    int8_t a[num_addr_to_send];
    for(j=0; j<num_addr_to_send; ++j){ 
      int i = j/addr_bytes;
      int k = j%addr_bytes;
      a[j] = (dest_scratch_addr[i] >> (k*8)) & 255; // since signed?
    }
    // if(j!=64) a[j]=-1; // delimeter

    addr_to_send = 1 | (1<<1) | (tag << 2) | (values_to_send << 18);
    // if(num_dest==1) std::cout << "Destination: " << multicast_dest[0] << "\n";
    spu_req_info req(num_dest, a, num_addr_to_send, addr_to_send, multicast_dest, req_type);
    // if(num_dest==1) std::cout << "Destination: " << req.mcast_dest[0] << "\n";
    push_net_req(req);
    dest_scratch_addr.clear();
    // std::cout << "Send tagged packet with num dest: " << num_dest << " tag: " << tag << " values bytes to wait for: " << values_to_send << " and num addr bytes: " << addr_to_send << " -1th bit: " << j << "\n";

  }

  // values
  int8_t v[values_to_send];
  // Step2: send the value packets
  if(num_dest!=0) {
    for(j=0; j<values_to_send; ++j){ 
      int i = j/val_bytes;
      int k = j%val_bytes;
      v[j] = (update_coalesce_vals[i] >> (k*8)) & 255;
    }
    addr_to_send = 1 | 0<<1 | tag << 2;
    // if(num_dest==1) std::cout << "Value Destination: " << multicast_dest[0] << "\n";
    spu_req_info req_seq(num_dest, v, values_to_send, addr_to_send, multicast_dest, req_type);
    // if(num_dest==1) std::cout << "Value Destination: " << req_seq.mcast_dest[0] << "\n";
    push_net_req(req_seq);
  }

  if(local_dest_scratch_addr.size()>0) {
    _last_tag = (_last_tag+1)%1024;
    tag = req_core << 10 | _last_tag;
    getSSIM().insert_pending_request_queue(tag, local_dest_scratch_addr, values_to_send);
    int num_addr = getSSIM().push_and_update_addr_in_pq(tag, values_to_send);
    std::vector<uint8_t> sent_val;
    for(int i=0; i<values_to_send; ++i) {
      sent_val.push_back(uint8_t(v[0]));
    }
    getSSIM().push_atomic_inc(tag, sent_val, num_addr);
  }

  return true;
}

// this should also broadcast updates (combine values with packets and limit
// broadcast to maximum allowed) -- scalar values can be upto 64-bits?
// TODO: need to make a plan for this...
bool Execute::push_scalar_rem_atom_op_req(uint64_t val, std::vector<int> update_broadcast_dest, std::vector<int> update_coalesce_vals, int opcode, int val_bytes, int out_bytes) {
  // FIXME: may not be applicable for others too (now I have move to delimeter
  // way)
  return false;

#if 0
  std::cout << "Received an atomic op request with dest size: " << update_broadcast_dest.size() << " and coalesce size: " << update_coalesce_vals.size() << "\n";

  assert(update_coalesce_vals.size()<=64/val_bytes && "cannot coalesce more than 64-byte update request");
  std::shared_ptr<SpuRequestMsg> msg = std::make_shared<SpuRequestMsg>(cpu.clockEdge());
  (*msg).m_MessageSize = MessageSizeType_Control;
  // (*msg).m_MessageSize = MessageSizeType_Response_Data;
  (*msg).m_Type = SpuRequestType_UPDATE;
  (*msg).m_Requestor = cpu.get_m_version();

  int num_updates = update_broadcast_dest.size();
  int num_vals = update_broadcast_dest.size();

  int num_dest=0;
  unsigned d=0;
  for(unsigned t=0; t<num_updates; t=d) { // should be tiled by a number
    // TODO: initialize new message with a new tid
    std::vector<int> dest_scratch_addr;
    // for(unsigned v=0;  v<num_vals; v+=4) {
      num_dest=0;
      for(d=t; num_dest<t+4 && d<num_updates; ++d) {
        int local_scr_addr = update_broadcast_dest[d] & (SCRATCH_SIZE-1);
        dest_scratch_addr.push_back(local_scr_addr);

        int dest_core_id = update_broadcast_dest[d]/SCRATCH_SIZE;
        
        dest_core_id += 1; // this should be 1 to 8

        // std::cout << "scr addr: " << local_scr_addr << " dest core id: " << dest_core_id << "\n";

        if(ssim.num_active_threads()==1 || dest_core_id==cpu.cpuId()) { // 0--host core when non-multi-threaded code
          DSA_LOG(NET_REQ) << "LOCAL REQUEST destination core: " << dest_core_id;
          ssim.push_atomic_update_req(local_scr_addr, opcode, val_bytes, out_bytes, val);
          continue;
         // return false;
        } 
        num_dest++;
        local_scr_addr = local_scr_addr & (SCRATCH_SIZE-1);

        // (*msg).m_addr = local_scr_addr | opcode << 16 | val_bytes << 18 | out_bytes << 20;
        // FIXME: assuming only banked scratchpad address space: take only last
        // 14-bits
        const char *col_map_str = std::getenv("COLMAP");
        bool col_map = false;
        if (col_map_str != NULL) {
          col_map = true;
        }
 
        if(col_map) {
          local_scr_addr = local_scr_addr & 1; // for just 2 cores
        } else {
          local_scr_addr = local_scr_addr & 16383;
        }
        (*msg).m_addr = local_scr_addr | opcode << 16 | val_bytes << 18 | out_bytes << 20;

        /*if(SS_DEBUG::NET_REQ){
           printf("output destination core: %d\n",dest_core_id);
          std::cout << "Atomic update net tuple, scr_addr: " << local_scr_addr << " and local core(0-indexed): " << cpu.cpuId() << " opcode: " << opcode << " val bytes: " << val_bytes << " out_bytes: " << out_bytes << std::endl; 
        }*/
        (*msg).m_Destination.add(cpu.get_m_version(dest_core_id));
      }
      if(num_dest!=0) {
        // TODO: send most addresses? (num_vals should reduce for the local
        // destinations)
        unsigned max_values_left =  num_vals*val_bytes; // std::min((num_vals-v)*val_bytes, unsigned(64));
        // get 4 values from update coalesce vals
        int8_t a[max_values_left];
        for(unsigned j=0; j<max_values_left; ++j){ 
          int i = j/val_bytes;
          int k = j%val_bytes;
          a[j] = (update_coalesce_vals[val+i] >> (k*8)) & 255;
        }
        // TODO: store dest scratch addr somewhere (giving seg fault)
        /*int addr_bits=16; // (32/16=2?)
        for(int j=0; j<num_dest; ++j) {
          (*msg).m_addr |= (update_broadcast_dest[j] << (j*addr_bits));
        }*/
        // std::cout << "Atomic update tupe for bytes: " << max_values_left << "\n";
        // TODO: no need to tiling in values, it will be split later
        spu_req_info req(msg, 1, a, max_values_left);
        push_net_req(req);
        dest_scratch_addr.clear();
      }
    // }
  }

 
  return true;
    /*
  // 50515 is definitely greater -- to test, I could use a smaller graph but
  //  mix of mapping for example, local_scr_addr >> 10 & (core_cnt-1);
  

  // always 16 or the same core, 6 = 110
  // because this is always in the local core, 
  // Okay I want to divide the available thing equally
  // 1 + 3352/8 = 1 + log(419) = 1+8.5 = 9.5
  // printf("received local scr addr: %ld\n",local_scr_addr);
  int dst_id = local_scr_addr/4;
  int vert_per_core = 3352/8;
  int dest_core_id = dst_id/vert_per_core;
  // int dest_core_id = local_scr_addr >> 9; // this should be 0 to 7
  // dest_core_id = local_scr_addr%ssim.num_active_threads(); // this should be 0 to 7
  // printf("src core: %d dest core: %d\n",cpu.cpuId(), dest_core_id);
  int p = rand()%32; // ssim.num_active_threads();
  if(p<31) dest_core_id = cpu.cpuId()-1;
  else dest_core_id = rand()%8; // ssim.num_active_threads();
*/


  // Ok, to maintain locality -- 
  // std::cout << "local scratch addr: " << local_scr_addr << std::endl;
  // dest_core_id = (local_scr_addr>>11)&7; // + rand()%2; // always even number
  // dest_core_id = (local_scr_addr/1583)&7; // + rand()%2; // always even number
  // std::cout << "dest core id: " << dest_core_id << std::endl;
  
  // TODO: scratch addr mapping (if we change this, then above decoding also
  // needs to change -- otherwise incorrect execution)
  // int dest_core_id = local_scr_addr >> 12 & 7;
  // int dest_core_id = rand()%1;
  // int dest_core_id = (local_scr_addr >> 1) & 1; // FIXME: just to debug
  /*if(SS_DEBUG::NET_REQ){
    printf("output destination core: %d\n",dest_core_id);
  }*/
  // FIXME: should not push more than 2 values in the same cycle
  /*if(cpu.cpuId()!=dest_core_id) {
    dest_core_id = 2; // it has to be 1
  }*/
// 
#endif
}

void Execute::send_spu_req(int src_port_id, int dest_port_id, int8_t* val, int num_bytes, uint64_t mask){
 
  int req_type=1;
  uint32_t addr_to_send = dest_port_id | num_bytes << 16; // TODO: encode data_width
  std::vector<int> mcast_dest;
  int dest_core_id = 0;
  std::bitset<64> core_mask(mask);
  bool should_send=false; int num_dest=0;
  for(int i=0; i<core_mask.size(); ++i){
    if(core_mask.test(i)){
      dest_core_id = i+1; // because of 1 offset with tid
        DSA_LOG(NET_REQ) << "output destinations: " << dest_core_id << " at core: " << cpu.cpuId();
      if(dest_core_id==cpu.cpuId() && dest_port_id!=src_port_id) {
        // send val to the dest port id
        ssim.push_in_accel_port(0, val, num_bytes, dest_port_id);
        DSA_LOG(NET_REQ) << "Local write at port_id: " << dest_port_id;
      } else { // no flag reqd, each core only covered once
        mcast_dest.push_back(dest_core_id);
        num_dest++;
        should_send = true;
      }
    }
  }
  // If all the requests were local?: Assuming this won't be the case
  if(should_send) {
    int mcast_destinations[num_dest];
    for(unsigned i=0; i<num_dest; ++i) {
      mcast_destinations[i]=mcast_dest[i];
    }
    spu_req_info req(num_dest, val, num_bytes, addr_to_send, mcast_destinations, req_type);
    push_net_req(req);
  }
}

void
Execute::evaluate()
{
    if (!inp.outputWire->isBubble())
        inputBuffer[inp.outputWire->threadId].setTail(*inp.outputWire);

    BranchData &branch = *out.inputWire;

    unsigned int num_issued = 0;

    /* Do all the cycle-wise activities for dcachePort here to potentially
     *  free up input spaces in the LSQ's requests queue */
    lsq.step();

    /* Let ssim tick for one cycle
     */

    bool ssim_done = !ssim.in_use(); //= ssim.done(false,0);
    if(!ssim_done) {
      ssim.step();
      cpu.activityRecorder->activity();
    }

    /* Check interrupts first.  Will halt commit if interrupt found */
    bool interrupted = false;
    ThreadID interrupt_tid = checkInterrupts(branch, interrupted);

    if (interrupt_tid != InvalidThreadID) {
        /* Signalling an interrupt this cycle, not issuing/committing from
         * any other threads */
    } else if (!branch.isBubble()) {
        /* It's important that this is here to carry Fetch1 wakeups to Fetch1
         *  without overwriting them */
        DPRINTF(MinorInterrupt, "Execute skipping a cycle to allow old"
            " branch to complete\n");
    } else {
        ThreadID commit_tid = getCommittingThread();

        if (commit_tid != InvalidThreadID) {
            ExecuteThreadInfo& commit_info = executeInfo[commit_tid];

            DPRINTF(MinorExecute, "Attempting to commit [tid:%d]\n",
                    commit_tid);
            /* commit can set stalled flags observable to issue and so *must* be
             *  called first */
            if (commit_info.drainState != NotDraining) {
                if (commit_info.drainState == DrainCurrentInst) {
                    /* Commit only micro-ops, don't kill anything else */
                    commit(commit_tid, true, false, branch);

                    if (isInbetweenInsts(commit_tid))
                        setDrainState(commit_tid, DrainHaltFetch);

                    /* Discard any generated branch */
                    branch = BranchData::bubble();
                } else if (commit_info.drainState == DrainAllInsts) {
                    /* Kill all instructions */
                    while (getInput(commit_tid))
                        popInput(commit_tid);
                    commit(commit_tid, false, true, branch);
                }
            } else {
                /* Commit micro-ops only if interrupted.  Otherwise, commit
                 *  anything you like */
                DPRINTF(MinorExecute, "Committing micro-ops for interrupt[tid:%d]\n",
                        commit_tid);
                bool only_commit_microops = interrupted &&
                                            hasInterrupt(commit_tid);
                commit(commit_tid, only_commit_microops, false, branch);
            }

            /* Halt fetch, but don't do it until we have the current instruction in
             *  the bag */
            if (commit_info.drainState == DrainHaltFetch) {
                updateBranchData(commit_tid, BranchData::HaltFetch,
                        MinorDynInst::bubble(), TheISA::PCState(0), branch);

                cpu.wakeupOnEvent(Pipeline::ExecuteStageId);
                setDrainState(commit_tid, DrainAllInsts);
            }
        }
        ThreadID issue_tid = getIssuingThread();
        /* This will issue merrily even when interrupted in the sure and
         *  certain knowledge that the interrupt with change the stream */
        if (issue_tid != InvalidThreadID) {
            DPRINTF(MinorExecute, "Attempting to issue [tid:%d]\n",
                    issue_tid);
            num_issued = issue(issue_tid);
        }

    }

    /* Run logic to step functional units + decide if we are active on the next
     * clock cycle */
    std::vector<MinorDynInstPtr> next_issuable_insts;
    bool can_issue_next = false;

    for (ThreadID tid = 0; tid < cpu.numThreads; tid++) {
        /* Find the next issuable instruction for each thread and see if it can
           be issued */
        if (getInput(tid)) {
            unsigned int input_index = executeInfo[tid].inputIndex;
            MinorDynInstPtr inst = getInput(tid)->insts[input_index];
            if (inst->isFault()) {
                can_issue_next = true;
            } else if (!inst->isBubble()) {
                next_issuable_insts.push_back(inst);
            }
        }
    }

    bool becoming_stalled = true;

    /* Advance the pipelines and note whether they still need to be
     * advanced */
    for (unsigned int i = 0; i < numFuncUnits; i++) {
        FUPipeline *fu = funcUnits[i];
        fu->advance();

        /* If we need to tick again, the pipeline will have been left or set
         * to be unstalled */
        if (fu->occupancy !=0 && !fu->stalled)
            becoming_stalled = false;

        /* Could we possibly issue the next instruction from any thread?
         * This is quite an expensive test and is only used to determine
         * if the CPU should remain active, only run it if we aren't sure
         * we are active next cycle yet */
        for (auto inst : next_issuable_insts) {
            if (!fu->stalled && fu->provides(inst->staticInst->opClass()) &&
                scoreboard[inst->id.threadId].canInstIssue(inst,
                    NULL, NULL, cpu.curCycle() + Cycles(1),
                    cpu.getContext(inst->id.threadId))) {
                can_issue_next = true;
                break;
            }
        }
    }

    bool head_inst_might_commit = false;

    /* Could the head in flight insts be committed */
    for (auto const &info : executeInfo) {
        if (!info.inFlightInsts->empty()) {
            const QueuedInst &head_inst = info.inFlightInsts->front();

            if (head_inst.inst->isNoCostInst()) {
                head_inst_might_commit = true;
            } else {
                FUPipeline *fu = funcUnits[head_inst.inst->fuIndex];
                if ((fu->stalled &&
                     fu->front().inst->id == head_inst.inst->id) ||
                     lsq.findResponse(head_inst.inst))
                {
                    head_inst_might_commit = true;
                    break;
                }
            }
        }
    }

    DPRINTF(Activity, "Need to tick num issued insts: %s%s%s%s%s%s%s\n",
       (!ssim_done ? "(ssim in use)" : ""),
       (num_issued != 0 ? " (issued some insts)" : ""),
       (becoming_stalled ? "(becoming stalled)" : "(not becoming stalled)"),
       (can_issue_next ? " (can issued next inst)" : ""),
       (head_inst_might_commit ? "(head inst might commit)" : ""),
       (lsq.needsToTick() ? " (LSQ needs to tick)" : ""),
       (interrupted ? " (interrupted)" : ""));

    bool need_to_tick =
       // !all_ssim_done || // this is on when there is global barrier
       !ssim_done || /* StreamSim is not done yet*/
       num_issued != 0 || /* Issued some insts this cycle */
       !becoming_stalled || /* Some FU pipelines can still move */
       can_issue_next || /* Can still issue a new inst */
       head_inst_might_commit || /* Could possible commit the next inst */
       lsq.needsToTick() || /* Must step the dcache port */
       interrupted; /* There are pending interrupts */

    if (!need_to_tick) {
        DPRINTF(Activity, "The next cycle might be skippable as there are no"
            " advanceable FUs\n");
    }

    /* Wake up if we need to tick again */
    if (need_to_tick)
        cpu.wakeupOnEvent(Pipeline::ExecuteStageId);

    /* Note activity of following buffer */
    if (!branch.isBubble())
        cpu.activityRecorder->activity();

    /* Make sure the input (if any left) is pushed */
    if (!inp.outputWire->isBubble())
        inputBuffer[inp.outputWire->threadId].pushTail();
}

ThreadID
Execute::checkInterrupts(BranchData& branch, bool& interrupted)
{
    ThreadID tid = interruptPriority;
    /* Evaluate interrupts in round-robin based upon service */
    do {
        /* Has an interrupt been signalled?  This may not be acted on
         *  straighaway so this is different from took_interrupt */
        bool thread_interrupted = false;

        if (FullSystem && cpu.getInterruptController(tid)) {
            /* This is here because it seems that after drainResume the
             * interrupt controller isn't always set */
            thread_interrupted = executeInfo[tid].drainState == NotDraining &&
                isInterrupted(tid);
            interrupted = interrupted || thread_interrupted;
        } else {
            DPRINTF(MinorInterrupt, "No interrupt controller\n");
        }
        DPRINTF(MinorInterrupt, "[tid:%d] thread_interrupted?=%d isInbetweenInsts?=%d\n",
                tid, thread_interrupted, isInbetweenInsts(tid));
        /* Act on interrupts */
        if (thread_interrupted && isInbetweenInsts(tid)) {
            if (takeInterrupt(tid, branch)) {
                interruptPriority = tid;
                return tid;
            }
        } else {
            tid = (tid + 1) % cpu.numThreads;
        }
    } while (tid != interruptPriority);

    return InvalidThreadID;
}

bool
Execute::hasInterrupt(ThreadID thread_id)
{
    if (FullSystem && cpu.getInterruptController(thread_id)) {
        return executeInfo[thread_id].drainState == NotDraining &&
               isInterrupted(thread_id);
    }

    return false;
}

void
Execute::minorTrace() const
{
    std::ostringstream insts;
    std::ostringstream stalled;

    executeInfo[0].instsBeingCommitted.reportData(insts);
    lsq.minorTrace();
    inputBuffer[0].minorTrace();
    scoreboard[0].minorTrace();

    /* Report functional unit stalling in one string */
    unsigned int i = 0;
    while (i < numFuncUnits)
    {
        stalled << (funcUnits[i]->stalled ? '1' : 'E');
        i++;
        if (i != numFuncUnits)
            stalled << ',';
    }

    MINORTRACE("insts=%s inputIndex=%d streamSeqNum=%d"
        " stalled=%s drainState=%d isInbetweenInsts=%d\n",
        insts.str(), executeInfo[0].inputIndex, executeInfo[0].streamSeqNum,
        stalled.str(), executeInfo[0].drainState, isInbetweenInsts(0));

    std::for_each(funcUnits.begin(), funcUnits.end(),
        std::mem_fun(&FUPipeline::minorTrace));

    executeInfo[0].inFlightInsts->minorTrace();
    executeInfo[0].inFUMemInsts->minorTrace();
}

inline ThreadID
Execute::getCommittingThread()
{
    std::vector<ThreadID> priority_list;

    switch (cpu.threadPolicy) {
      case Enums::SingleThreaded:
          return 0;
      case Enums::RoundRobin:
          priority_list = cpu.roundRobinPriority(commitPriority);
          break;
      case Enums::Random:
          priority_list = cpu.randomPriority();
          break;
      default:
          panic("Invalid thread policy");
    }

    for (auto tid : priority_list) {

        ExecuteThreadInfo &ex_info = executeInfo[tid];
        // bool can_commit_insts = !ex_info.inFlightInsts->empty();
        bool is_thread_active =
                cpu.getContext(tid)->status() == ThreadContext::Active;
        bool can_commit_insts = !ex_info.inFlightInsts->empty() &&
                                is_thread_active;

        if (can_commit_insts) {
            QueuedInst *head_inflight_inst = &(ex_info.inFlightInsts->front());
            MinorDynInstPtr inst = head_inflight_inst->inst;

            can_commit_insts = can_commit_insts &&
                (!inst->inLSQ || (lsq.findResponse(inst) != NULL));

            if (!inst->inLSQ) {
                bool can_transfer_mem_inst = false;
                if (!ex_info.inFUMemInsts->empty() && lsq.canRequest()) {
                    const MinorDynInstPtr head_mem_ref_inst =
                        ex_info.inFUMemInsts->front().inst;
                    FUPipeline *fu = funcUnits[head_mem_ref_inst->fuIndex];
                    const MinorDynInstPtr &fu_inst = fu->front().inst;
                    can_transfer_mem_inst =
                        !fu_inst->isBubble() &&
                         fu_inst->id.threadId == tid &&
                         !fu_inst->inLSQ &&
                         fu_inst->canEarlyIssue &&
                         inst->id.execSeqNum > fu_inst->instToWaitFor;
                }

                bool can_execute_fu_inst = inst->fuIndex == noCostFUIndex;
                if (can_commit_insts && !can_transfer_mem_inst &&
                        inst->fuIndex != noCostFUIndex)
                {
                    QueuedInst& fu_inst = funcUnits[inst->fuIndex]->front();
                    can_execute_fu_inst = !fu_inst.inst->isBubble() &&
                        fu_inst.inst->id == inst->id;
                }

                can_commit_insts = can_commit_insts &&
                    (can_transfer_mem_inst || can_execute_fu_inst);
            }
        }


        if (can_commit_insts) {
            commitPriority = tid;
            return tid;
        }
    }

    return InvalidThreadID;
}

inline ThreadID
Execute::getIssuingThread()
{
    std::vector<ThreadID> priority_list;

    switch (cpu.threadPolicy) {
      case Enums::SingleThreaded:
          return 0;
      case Enums::RoundRobin:
          priority_list = cpu.roundRobinPriority(issuePriority);
          break;
      case Enums::Random:
          priority_list = cpu.randomPriority();
          break;
      default:
          panic("Invalid thread scheduling policy.");
    }

    for (auto tid : priority_list) {
        // if (getInput(tid)) {
        if (cpu.getContext(tid)->status() == ThreadContext::Active &&
            getInput(tid)) {
            issuePriority = tid;
            return tid;
        }
    }

    return InvalidThreadID;
}

void
Execute::drainResume()
{
    DPRINTF(Drain, "MinorExecute drainResume\n");

    for (ThreadID tid = 0; tid < cpu.numThreads; tid++) {
        setDrainState(tid, NotDraining);
    }

    cpu.wakeupOnEvent(Pipeline::ExecuteStageId);
}

std::ostream &operator <<(std::ostream &os, Execute::DrainState state)
{
    switch (state)
    {
        case Execute::NotDraining:
          os << "NotDraining";
          break;
        case Execute::DrainCurrentInst:
          os << "DrainCurrentInst";
          break;
        case Execute::DrainHaltFetch:
          os << "DrainHaltFetch";
          break;
        case Execute::DrainAllInsts:
          os << "DrainAllInsts";
          break;
        default:
          os << "Drain-" << static_cast<int>(state);
          break;
    }

    return os;
}

void
Execute::setDrainState(ThreadID thread_id, DrainState state)
{
    DPRINTF(Drain, "setDrainState[%d]: %s\n", thread_id, state);
    executeInfo[thread_id].drainState = state;
}

unsigned int
Execute::drain()
{
    DPRINTF(Drain, "MinorExecute drain\n");

    for (ThreadID tid = 0; tid < cpu.numThreads; tid++) {
        if (executeInfo[tid].drainState == NotDraining) {
            cpu.wakeupOnEvent(Pipeline::ExecuteStageId);

            /* Go to DrainCurrentInst if we're between microops
             * or waiting on an unbufferable memory operation.
             * Otherwise we can go straight to DrainHaltFetch
             */
            if (isInbetweenInsts(tid))
                setDrainState(tid, DrainHaltFetch);
            else
                setDrainState(tid, DrainCurrentInst);
        }
    }
    return (isDrained() ? 0 : 1);
}

bool
Execute::isDrained()
{
    if (!lsq.isDrained())
        return false;

    for (ThreadID tid = 0; tid < cpu.numThreads; tid++) {
        if (!inputBuffer[tid].empty() ||
            !executeInfo[tid].inFlightInsts->empty()) {

            return false;
        }
    }

    return true;
}

Execute::~Execute()
{
    for (unsigned int i = 0; i < numFuncUnits; i++)
        delete funcUnits[i];

    for (ThreadID tid = 0; tid < cpu.numThreads; tid++)
        delete executeInfo[tid].inFlightInsts;
}

bool
Execute::instIsRightStream(MinorDynInstPtr inst)
{
    return inst->id.streamSeqNum == executeInfo[inst->id.threadId].streamSeqNum;
}

bool
Execute::instIsHeadInst(MinorDynInstPtr inst)
{
    bool ret = false;

    if (!executeInfo[inst->id.threadId].inFlightInsts->empty())
        ret = executeInfo[inst->id.threadId].inFlightInsts->front().inst->id == inst->id;

    return ret;
}

MinorCPU::MinorCPUPort &
Execute::getDcachePort()
{
    return lsq.getDcachePort();
}

}
