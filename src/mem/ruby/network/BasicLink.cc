/*
 * Copyright (c) 2011 Advanced Micro Devices, Inc.
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
 */

#include "mem/ruby/network/BasicLink.hh"

BasicLink::BasicLink(const Params *p)
    : SimObject(p)
{
    m_latency = p->latency;
    m_bandwidth_factor = p->bandwidth_factor;
    m_weight = p->weight;
}

void
BasicLink::init()
{
}

void
BasicLink::print(std::ostream& out) const
{
    out << name();
}

BasicLink *
BasicLinkParams::create()
{
    return new BasicLink(this);
}

BasicExtLink::BasicExtLink(const Params *p)
    : BasicLink(p)
{
}

BasicExtLink *
BasicExtLinkParams::create()
{
    return new BasicExtLink(this);
}

SpuExtLink::SpuExtLink(const Params *p)
    : BasicLink(p)
{
}

SpuExtLink *
SpuExtLinkParams::create()
{
    return new SpuExtLink(this);
}

BasicIntLink::BasicIntLink(const Params *p)
    : BasicLink(p)
{
}

BasicIntLink *
BasicIntLinkParams::create()
{
    return new BasicIntLink(this);
}
