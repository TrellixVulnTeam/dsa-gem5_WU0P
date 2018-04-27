/*
 * Copyright (c) 2011-2014 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
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
 * Copyright (c) 2002-2005 The Regents of The University of Michigan
 * All rights reserved.
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
 * Authors: Steve Reinhardt
 *          Dave Greene
 *          Nathan Binkert
 *          Andrew Bardsley
 */

/**
 * @file
 *
 *  ExecContext bears the exec_context interface for Minor.
 */

#ifndef __CPU_MINOR_EXEC_CONTEXT_HH__
#define __CPU_MINOR_EXEC_CONTEXT_HH__

#include "cpu/base.hh"
#include "cpu/exec_context.hh"
#include "cpu/minor/execute.hh"
#include "cpu/minor/pipeline.hh"
#include "cpu/simple_thread.hh"
#include "ssim.hh"
#include "debug/MinorExecute.hh"
#include "debug/SD.hh"
#include "mem/request.hh"
#include "cpu/sd_regs.hh"

namespace Minor
{

/* Forward declaration of Execute */
class Execute;

/** ExecContext bears the exec_context interface for Minor.  This nicely
 *  separates that interface from other classes such as Pipeline, MinorCPU
 *  and DynMinorInst and makes it easier to see what state is accessed by it.
 */
class ExecContext : public ::ExecContext
{
  public:
    MinorCPU &cpu;

    /** ThreadState object, provides all the architectural state. */
    SimpleThread &thread;

    /** The execute stage so we can peek at its contents. */
    Execute &execute;

    /** Instruction for the benefit of memory operations and for PC */
    MinorDynInstPtr inst;

    ExecContext (
        MinorCPU &cpu_,
        SimpleThread &thread_, Execute &execute_,
        MinorDynInstPtr inst_) :
        cpu(cpu_),
        thread(thread_),
        execute(execute_),
        inst(inst_)
    {
        DPRINTF(MinorExecute, "ExecContext setting PC: %s\n", inst->pc);
        pcState(inst->pc);
        setPredicate(true);
        thread.setIntReg(TheISA::ZeroReg, 0);
#if THE_ISA == ALPHA_ISA
        thread.setFloatReg(TheISA::ZeroReg, 0.0);
#endif
    }

    Fault
    initiateMemRead(Addr addr, unsigned int size,
                    Request::Flags flags) override
    {
        execute.getLSQ().pushRequest(inst, true /* load */, nullptr,
            size, addr, flags, NULL);
        return NoFault;
    }

    Fault
    writeMem(uint8_t *data, unsigned int size, Addr addr,
             Request::Flags flags, uint64_t *res) override
    {
        execute.getLSQ().pushRequest(inst, false /* store */, data,
            size, addr, flags, res);
        return NoFault;
    }

    IntReg
    readIntRegOperand(const StaticInst *si, int idx) override
    {
        return thread.readIntReg(si->srcRegIdx(idx));
    }

    TheISA::FloatReg
    readFloatRegOperand(const StaticInst *si, int idx) override
    {
        int reg_idx = si->srcRegIdx(idx) - TheISA::FP_Reg_Base;
        return thread.readFloatReg(reg_idx);
    }

    TheISA::FloatRegBits
    readFloatRegOperandBits(const StaticInst *si, int idx) override
    {
        int reg_idx = si->srcRegIdx(idx) - TheISA::FP_Reg_Base;
        return thread.readFloatRegBits(reg_idx);
    }

    void
    setIntRegOperand(const StaticInst *si, int idx, IntReg val) override
    {
        thread.setIntReg(si->destRegIdx(idx), val);
    }

    void
    setFloatRegOperand(const StaticInst *si, int idx,
        TheISA::FloatReg val) override
    {
        int reg_idx = si->destRegIdx(idx) - TheISA::FP_Reg_Base;
        thread.setFloatReg(reg_idx, val);
    }

    void
    setFloatRegOperandBits(const StaticInst *si, int idx,
        TheISA::FloatRegBits val) override
    {
        int reg_idx = si->destRegIdx(idx) - TheISA::FP_Reg_Base;
        thread.setFloatRegBits(reg_idx, val);
    }

    bool
    readPredicate() override
    {
        return thread.readPredicate();
    }

    void
    setPredicate(bool val) override
    {
        thread.setPredicate(val);
    }

    TheISA::PCState
    pcState() const override
    {
        return thread.pcState();
    }

    void
    pcState(const TheISA::PCState &val) override
    {
        thread.pcState(val);
    }

    TheISA::MiscReg
    readMiscRegNoEffect(int misc_reg) const
    {
        return thread.readMiscRegNoEffect(misc_reg);
    }

    TheISA::MiscReg
    readMiscReg(int misc_reg) override
    {
        return thread.readMiscReg(misc_reg);
    }

    void
    setMiscReg(int misc_reg, const TheISA::MiscReg &val) override
    {
        thread.setMiscReg(misc_reg, val);
    }

    TheISA::MiscReg
    readMiscRegOperand(const StaticInst *si, int idx) override
    {
        int reg_idx = si->srcRegIdx(idx) - TheISA::Misc_Reg_Base;
        return thread.readMiscReg(reg_idx);
    }

    void
    setMiscRegOperand(const StaticInst *si, int idx,
        const TheISA::MiscReg &val) override
    {
        int reg_idx = si->destRegIdx(idx) - TheISA::Misc_Reg_Base;
        return thread.setMiscReg(reg_idx, val);
    }

    Fault
    hwrei() override
    {
#if THE_ISA == ALPHA_ISA
        return thread.hwrei();
#else
        return NoFault;
#endif
    }

    bool
    simPalCheck(int palFunc) override
    {
#if THE_ISA == ALPHA_ISA
        return thread.simPalCheck(palFunc);
#else
        return false;
#endif
    }

    void
    syscall(int64_t callnum, Fault *fault) override
     {
        if (FullSystem)
            panic("Syscall emulation isn't available in FS mode.\n");

        thread.syscall(callnum, fault);
    }

    ThreadContext *tcBase() override { return thread.getTC(); }

    /* @todo, should make stCondFailures persistent somewhere */
    unsigned int readStCondFailures() const override { return 0; }
    void setStCondFailures(unsigned int st_cond_failures) override {}

    ContextID contextId() { return thread.contextId(); }
    /* ISA-specific (or at least currently ISA singleton) functions */

    /* X86: TLB twiddling */
    void
    demapPage(Addr vaddr, uint64_t asn) override
    {
        thread.getITBPtr()->demapPage(vaddr, asn);
        thread.getDTBPtr()->demapPage(vaddr, asn);
    }

    TheISA::CCReg
    readCCRegOperand(const StaticInst *si, int idx) override
    {
        int reg_idx = si->srcRegIdx(idx) - TheISA::CC_Reg_Base;
        return thread.readCCReg(reg_idx);
    }

    void
    setCCRegOperand(const StaticInst *si, int idx, TheISA::CCReg val) override
    {
        int reg_idx = si->destRegIdx(idx) - TheISA::CC_Reg_Base;
        thread.setCCReg(reg_idx, val);
    }

    void
    demapInstPage(Addr vaddr, uint64_t asn)
    {
        thread.getITBPtr()->demapPage(vaddr, asn);
    }

    void
    demapDataPage(Addr vaddr, uint64_t asn)
    {
        thread.getDTBPtr()->demapPage(vaddr, asn);
    }

    /* ALPHA/POWER: Effective address storage */
    void setEA(Addr ea) override
    {
        inst->ea = ea;
    }

    BaseCPU *getCpuPtr() { return &cpu; }

    /* POWER: Effective address storage */
    Addr getEA() const override
    {
        return inst->ea;
    }

    /* MIPS: other thread register reading/writing */
    uint64_t
    readRegOtherThread(int idx, ThreadID tid = InvalidThreadID)
    {
        SimpleThread *other_thread = (tid == InvalidThreadID
            ? &thread : cpu.threads[tid]);

        if (idx < TheISA::FP_Reg_Base) { /* Integer */
            return other_thread->readIntReg(idx);
        } else if (idx < TheISA::Misc_Reg_Base) { /* Float */
            return other_thread->readFloatRegBits(idx
                - TheISA::FP_Reg_Base);
        } else { /* Misc */
            return other_thread->readMiscReg(idx
                - TheISA::Misc_Reg_Base);
        }
    }

    void
    setRegOtherThread(int idx, const TheISA::MiscReg &val,
        ThreadID tid = InvalidThreadID)
    {
        SimpleThread *other_thread = (tid == InvalidThreadID
            ? &thread : cpu.threads[tid]);

        if (idx < TheISA::FP_Reg_Base) { /* Integer */
            return other_thread->setIntReg(idx, val);
        } else if (idx < TheISA::Misc_Reg_Base) { /* Float */
            return other_thread->setFloatRegBits(idx
                - TheISA::FP_Reg_Base, val);
        } else { /* Misc */
            return other_thread->setMiscReg(idx
                - TheISA::Misc_Reg_Base, val);
        }
    }

  public:
    // monitor/mwait funtions
    void armMonitor(Addr address) override
    { getCpuPtr()->armMonitor(inst->id.threadId, address); }

    bool mwait(PacketPtr pkt) override
    { return getCpuPtr()->mwait(inst->id.threadId, pkt); }

    void mwaitAtomic(ThreadContext *tc) override
    { return getCpuPtr()->mwaitAtomic(inst->id.threadId, tc, thread.dtb); }

    AddressMonitor *getAddrMonitor() override
    { return getCpuPtr()->getCpuAddrMonitor(inst->id.threadId); }


#ifdef ISA_HAS_SD
    uint64_t receiveSD() {
      DPRINTF(SD, "Do SD_COMMAND RECEIVE\n");
      ssim_t& ssim = execute.getSSIM();
      return ssim.receive(thread.getSDReg(SD_OUT_PORT));
    }

    void setSDReg(uint64_t val, int sd_idx) {
        thread.setSDReg(val, sd_idx);
    }
    void callSDFunc(int sd_func_opcode) {
        DPRINTF(SD, "Do SD_COMMAND %d.\n", SDCmdNames[sd_func_opcode]);
        ssim_t& ssim = execute.getSSIM();
        ssim.set_cur_minst(inst);
        switch(sd_func_opcode) {
            case SB_BEGIN_ROI: ssim.roi_entry(true); break;
            case SB_END_ROI: ssim.roi_entry(false); break;
            case SB_STATS: ssim.print_stats(); break;
            case SB_CFG: ssim.req_config(
                thread.getSDReg(SD_MEM_ADDR),      thread.getSDReg(SD_CFG_SIZE)); 
            break;
            //case SB_CFG_PORT: ssim.cfg_port(
            //    thread.getSDReg(SD_CONSTANT),      thread.getSDReg(SD_IN_PORT)); 
            //break;
            case SB_CTX: ssim.set_context(thread.getSDReg(SD_CONTEXT)); 
            break;
            case SB_FILL_MODE: ssim.set_fill_mode(thread.getSDReg(SD_CONSTANT)); 
            break;
            case SB_MEM_SCR: ssim.load_dma_to_scratch(
                thread.getSDReg(SD_MEM_ADDR),      thread.getSDReg(SD_STRIDE),
                thread.getSDReg(SD_ACCESS_SIZE),   thread.getSDReg(SD_STRETCH),   
                thread.getSDReg(SD_NUM_STRIDES),   thread.getSDReg(SD_SCRATCH_ADDR),
                thread.getSDReg(SD_FLAGS)); 
            break;
            case SB_SCR_MEM: ssim.write_dma_from_scratch(
                thread.getSDReg(SD_SCRATCH_ADDR),      thread.getSDReg(SD_STRIDE),
                thread.getSDReg(SD_ACCESS_SIZE),   thread.getSDReg(SD_NUM_STRIDES),
                thread.getSDReg(SD_MEM_ADDR), thread.getSDReg(SD_FLAGS)); 
            break;
            case SB_MEM_PRT: ssim.load_dma_to_port(
                thread.getSDReg(SD_MEM_ADDR),      thread.getSDReg(SD_STRIDE),
                thread.getSDReg(SD_ACCESS_SIZE),   thread.getSDReg(SD_STRETCH), 
                thread.getSDReg(SD_NUM_STRIDES),   thread.getSDReg(SD_IN_PORT),       
                thread.getSDReg(SD_REPEAT), thread.getSDReg(SD_REPEAT_STRETCH));      
            break;
            case SB_SCR_PRT: ssim.load_scratch_to_port(
                thread.getSDReg(SD_SCRATCH_ADDR), thread.getSDReg(SD_STRIDE),
                thread.getSDReg(SD_ACCESS_SIZE),  thread.getSDReg(SD_STRETCH),   
                thread.getSDReg(SD_NUM_STRIDES),  thread.getSDReg(SD_IN_PORT),
                thread.getSDReg(SD_REPEAT), thread.getSDReg(SD_REPEAT_STRETCH));
            break;
            case SB_PRT_SCR: ssim.write_scratchpad(
                thread.getSDReg(SD_OUT_PORT), thread.getSDReg(SD_SCRATCH_ADDR),  
                thread.getSDReg(SD_NUM_BYTES), thread.getSDReg(SD_SHIFT_BYTES));      
            break;
            case SB_PRT_MEM: ssim.write_dma(
                thread.getSDReg(SD_GARB_ELEM),
                thread.getSDReg(SD_OUT_PORT),      thread.getSDReg(SD_STRIDE),
                thread.getSDReg(SD_ACCESS_SIZE),   thread.getSDReg(SD_NUM_STRIDES),
                thread.getSDReg(SD_MEM_ADDR),      thread.getSDReg(SD_SHIFT_BYTES),
                thread.getSDReg(SD_GARBAGE));
            break;
            case SB_PRT_PRT: ssim.reroute(
                thread.getSDReg(SD_OUT_PORT),      thread.getSDReg(SD_IN_PORT),
                thread.getSDReg(SD_NUM_ELEM),      thread.getSDReg(SD_REPEAT), 
                thread.getSDReg(SD_REPEAT_STRETCH),
                thread.getSDReg(SD_FLAGS));     
            break;
            case SB_IND_PRT: ssim.indirect(
                thread.getSDReg(SD_IND_PORT),      thread.getSDReg(SD_IND_TYPE),
                thread.getSDReg(SD_IN_PORT),       thread.getSDReg(SD_INDEX_ADDR),
                thread.getSDReg(SD_NUM_ELEM),      thread.getSDReg(SD_REPEAT),
                thread.getSDReg(SD_REPEAT_STRETCH),thread.getSDReg(SD_OFFSET_LIST),
                thread.getSDReg(SD_DTYPE),         thread.getSDReg(SD_IND_MULT));
            break;
            case SB_PRT_IND: ssim.indirect_write(
                thread.getSDReg(SD_IND_PORT),       thread.getSDReg(SD_IND_TYPE),
                thread.getSDReg(SD_OUT_PORT),       thread.getSDReg(SD_INDEX_ADDR),
                thread.getSDReg(SD_NUM_ELEM),       thread.getSDReg(SD_OFFSET_LIST),
                thread.getSDReg(SD_DTYPE),         thread.getSDReg(SD_IND_MULT));
            break;
            case SB_CNS_PRT: ssim.write_constant(
                thread.getSDReg(SD_NUM_STRIDES),   thread.getSDReg(SD_IN_PORT),
                thread.getSDReg(SD_CONSTANT),      thread.getSDReg(SD_NUM_ELEM),     
                thread.getSDReg(SD_CONSTANT2),     thread.getSDReg(SD_NUM_ELEM2),      
                thread.getSDReg(SD_FLAGS) );
            break;
            case SB_WAIT:
                if(thread.getSDReg(SD_WAIT_MASK) == 0) {
                    ssim.set_not_in_use();
                    DPRINTF(SD, "Set SB Not in Use\n");
                } else if(thread.getSDReg(SD_WAIT_MASK) == 2) {
                     DPRINTF(SD, "Wait Compute\n");         
                } else if(thread.getSDReg(SD_WAIT_MASK) == 16) {
                     DPRINTF(SD, "Wait mem write\n");
                } else {
                     ssim.insert_barrier(thread.getSDReg(SD_WAIT_MASK));
                }
            break;
            default:
                DPRINTF(SD, "UNIMPLEMENTED COMMAND\n");
                break;
        }
        //RESET REPEAT to 1 -- since this is by far the most common case
        setSDReg(1,SD_REPEAT);
        setSDReg(0,SD_REPEAT_STRETCH);
        setSDReg(0,SD_OFFSET_LIST);
        setSDReg(0,SD_IND_TYPE);
        setSDReg(0,SD_DTYPE);
        setSDReg(1,SD_IND_MULT);

    }
#endif

};

}

#endif /* __CPU_MINOR_EXEC_CONTEXT_HH__ */
