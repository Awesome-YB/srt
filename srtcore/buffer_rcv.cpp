/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2018 Haivision Systems Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

/*****************************************************************************
Copyright (c) 2001 - 2009, The Board of Trustees of the University of Illinois.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the
  above copyright notice, this list of conditions
  and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the University of Illinois
  nor the names of its contributors may be used to
  endorse or promote products derived from this
  software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*****************************************************************************/

#include <cmath>
#include <limits>
#include "buffer_rcv.h"
#include "logging.h"

using namespace std;

using namespace srt::sync;
using namespace srt_logging;
namespace srt_logging
{
    extern Logger brlog;
}
#define rbuflog brlog

namespace srt {

namespace {
    struct ScopedLog
    {
        ScopedLog() {}

        ~ScopedLog()
        {
            LOGC(rbuflog.Warn, log << ss.str());
        }

        stringstream ss;
    };

#define IF_RCVBUF_DEBUG(instr) (void)0

}


/*
 *   RcvBufferNew (circular buffer):
 *
 *   |<------------------- m_iSize ----------------------------->|
 *   |       |<----------- m_iMaxPosOff ------------>|           |
 *   |       |                                       |           |
 *   +---+---+---+---+---+---+---+---+---+---+---+---+---+   +---+
 *   | 0 | 0 | 1 | 1 | 1 | 0 | 1 | 1 | 1 | 1 | 0 | 1 | 0 |...| 0 | m_pUnit[]
 *   +---+---+---+---+---+---+---+---+---+---+---+---+---+   +---+
 *             |                                   |
 *             |                                   |__last pkt received
 *             |___ m_iStartPos: first message to read
 *
 *   m_pUnit[i]->m_iFlag: 0:free, 1:good, 2:passack, 3:dropped
 *
 *   thread safety:
 *    m_iStartPos:   CUDT::m_RecvLock
 *    m_iLastAckPos: CUDT::m_AckLock
 *    m_iMaxPosOff:     none? (modified on add and ack
 */

CRcvBuffer::CRcvBuffer(int initSeqNo, size_t size, CUnitQueue* unitqueue, bool bMessageAPI)
    : m_entries(size)
    , m_szSize(size) // TODO: maybe just use m_entries.size()
    , m_pUnitQueue(unitqueue)
    , m_iStartSeqNo(initSeqNo) // NOTE: SRT_SEQNO_NONE is allowed here.
    , m_iStartPos(&m_szSize, 0)
    , m_iEndPos(&m_szSize, 0)
    , m_iDropPos(&m_szSize, 0)
    , m_iFirstNonreadPos(&m_szSize, 0)
    , m_iMaxPosOff(0)
    , m_iNotch(0)
    , m_numNonOrderPackets(0)
    , m_iFirstNonOrderMsgPos(&m_szSize, CPos_TRAP.val())
    , m_bPeerRexmitFlag(true)
    , m_bMessageAPI(bMessageAPI)
    , m_iBytesCount(0)
    , m_iPktsCount(0)
    , m_uAvgPayloadSz(0)
{
    SRT_ASSERT(size < size_t(std::numeric_limits<int>::max())); // All position pointers are integers
}

CRcvBuffer::~CRcvBuffer()
{
    // Can be optimized by only iterating m_iMaxPosOff from m_iStartPos.
    for (FixedArray<Entry>::iterator it = m_entries.begin(); it != m_entries.end(); ++it)
    {
        if (!it->pUnit)
            continue;

        m_pUnitQueue->makeUnitFree(it->pUnit);
        it->pUnit = NULL;
    }
}

void CRcvBuffer::debugShowState(const char* source SRT_ATR_UNUSED)
{
    HLOGC(brlog.Debug, log << "RCV-BUF-STATE(" << source
            << ") start=" << m_iStartPos VALUE
            << " end=" << m_iEndPos VALUE
            << " drop=" << m_iDropPos VALUE
            << " max-off=+" << m_iMaxPosOff VALUE
            << " seq[start]=%" << m_iStartSeqNo VALUE);
}

CRcvBuffer::InsertInfo CRcvBuffer::insert(CUnit* unit)
{
    SRT_ASSERT(unit != NULL);
    const int32_t seqno  = unit->m_Packet.getSeqNo();
    //const int     offset = CSeqNo::seqoff(m_iStartSeqNo, seqno);
    const COff offset = COff(CSeqNo(seqno) - m_iStartSeqNo);

    IF_RCVBUF_DEBUG(ScopedLog scoped_log);
    IF_RCVBUF_DEBUG(scoped_log.ss << "CRcvBuffer::insert: seqno " << seqno);
    IF_RCVBUF_DEBUG(scoped_log.ss << " msgno " << unit->m_Packet.getMsgSeq(m_bPeerRexmitFlag));
    IF_RCVBUF_DEBUG(scoped_log.ss << " m_iStartSeqNo " << m_iStartSeqNo << " offset " << offset);

    if (offset < COff(0))
    {
        IF_RCVBUF_DEBUG(scoped_log.ss << " returns -2");
        return InsertInfo(InsertInfo::BELATED);
    }
    IF_HEAVY_LOGGING(string debug_source = "insert %" + Sprint(seqno));

    if (offset >= COff(capacity()))
    {
        IF_RCVBUF_DEBUG(scoped_log.ss << " returns -3");

        InsertInfo ireport (InsertInfo::DISCREPANCY);
        getAvailInfo((ireport));

        IF_HEAVY_LOGGING(debugShowState((debug_source + " overflow").c_str()));

        return ireport;
    }

    // TODO: Don't do assert here. Process this situation somehow.
    // If >= 2, then probably there is a long gap, and buffer needs to be reset.
    SRT_ASSERT((m_iStartPos VALUE + offset VALUE) / m_szSize < 2);

    //const CPos newpktpos = m_iStartPos + offset;
    const CPos newpktpos = incPos(m_iStartPos, offset);
    const COff prev_max_off = m_iMaxPosOff;
    bool extended_end = false;
    if (offset >= m_iMaxPosOff)
    {
        m_iMaxPosOff = offset + COff(1);
        extended_end = true;
    }

    // Packet already exists
    // (NOTE: the above extension of m_iMaxPosOff is
    // possible even before checking that the packet
    // exists because existence of a packet beyond
    // the current max position is not possible).
    SRT_ASSERT(newpktpos VALUE >= 0 && newpktpos VALUE < int(m_szSize));
    if (m_entries[newpktpos].status != EntryState_Empty)
    {
        IF_RCVBUF_DEBUG(scoped_log.ss << " returns -1");
        IF_HEAVY_LOGGING(debugShowState((debug_source + " redundant").c_str()));
        return InsertInfo(InsertInfo::REDUNDANT);
    }
    SRT_ASSERT(m_entries[newpktpos].pUnit == NULL);

    m_pUnitQueue->makeUnitTaken(unit);
    m_entries[newpktpos].pUnit  = unit;
    m_entries[newpktpos].status = EntryState_Avail;
    countBytes(1, (int)unit->m_Packet.getLength());

    // Set to a value, if due to insertion there was added
    // a packet that is earlier to be retrieved than the earliest
    // currently available packet.
    time_point earlier_time = updatePosInfo(unit, prev_max_off, newpktpos, extended_end);

    InsertInfo ireport (InsertInfo::INSERTED);
    ireport.first_time = earlier_time;

    // If packet "in order" flag is zero, it can be read out of order.
    // With TSBPD enabled packets are always assumed in order (the flag is ignored).
    if (!m_tsbpd.isEnabled() && m_bMessageAPI && !unit->m_Packet.getMsgOrderFlag())
    {
        ++m_numNonOrderPackets;
        onInsertNonOrderPacket(newpktpos);
    }

    updateNonreadPos();

    // This updates only the first_seq and avail_range fields.
    getAvailInfo((ireport));

    IF_RCVBUF_DEBUG(scoped_log.ss << " returns 0 (OK)");
    IF_HEAVY_LOGGING(debugShowState((debug_source + " ok").c_str()));

    return ireport;
}

void CRcvBuffer::getAvailInfo(CRcvBuffer::InsertInfo& w_if)
{
   CPos fallback_pos = CPos_TRAP;
   if (!m_tsbpd.isEnabled())
   {
       // In case when TSBPD is off, we take into account the message mode
       // where messages may potentially span for multiple packets, therefore
       // the only "next deliverable" is the first complete message that satisfies
       // the order requirement.
       // NOTE THAT this field can as well be -1 already.
       fallback_pos = m_iFirstNonOrderMsgPos;
   }
   else if (m_iDropPos != m_iEndPos)
   {
       // With TSBPD regard the drop position (regardless if
       // TLPKTDROP is currently on or off), if "exists", that
       // is, m_iDropPos != m_iEndPos.
       fallback_pos = m_iDropPos;
   }

   // This finds the first possible available packet, which is
   // preferably at cell 0, but if not available, try also with
   // given fallback position (unless it's -1).
   const CPacket* pkt = tryAvailPacketAt(fallback_pos, (w_if.avail_range));
   if (pkt)
   {
       w_if.first_seq = CSeqNo(pkt->getSeqNo());
   }
}


const CPacket* CRcvBuffer::tryAvailPacketAt(CPos pos, COff& w_span)
{
   if (m_entries[m_iStartPos].status == EntryState_Avail)
   {
       pos = m_iStartPos;
       w_span = offPos(m_iStartPos, m_iEndPos);
       //w_span = m_iEndPos - m_iStartPos;
   }

   if (pos == CPos_TRAP)
   {
       w_span = COff(0);
       return NULL;
   }

   SRT_ASSERT(m_entries[pos].pUnit != NULL);

   // TODO: we know that at least 1 packet is available, but only
   // with m_iEndPos we know where the true range is. This could also
   // be implemented for message mode, but still this would employ
   // a separate begin-end range declared for a complete out-of-order
   // message.
   w_span = COff(1);
   return &packetAt(pos);
}

CRcvBuffer::time_point CRcvBuffer::updatePosInfo(const CUnit* unit, const COff prev_max_off, const CPos newpktpos, const bool extended_end)
{
   time_point earlier_time;

   CPos prev_max_pos = incPos(m_iStartPos, prev_max_off);
   //CPos prev_max_pos = m_iStartPos + prev_max_off;

   // Update flags
   // Case [A]
   if (extended_end)
   {
       // THIS means that the buffer WAS CONTIGUOUS BEFORE.
       if (m_iEndPos == prev_max_pos)
       {
           // THIS means that the new packet didn't CAUSE a gap
           if (m_iMaxPosOff == prev_max_off + 1)
           {
               // This means that m_iEndPos now shifts by 1,
               // and m_iDropPos must be shifted together with it,
               // as there's no drop to point.
               m_iEndPos = incPos(m_iStartPos, m_iMaxPosOff);
               //m_iEndPos = m_iStartPos + m_iMaxPosOff;
               m_iDropPos = m_iEndPos;
           }
           else
           {
               // Otherwise we have a drop-after-gap candidate
               // which is the currently inserted packet.
               // Therefore m_iEndPos STAYS WHERE IT IS.
               m_iDropPos = incPos(m_iStartPos, m_iMaxPosOff - 1);
               //m_iDropPos = m_iStartPos + (m_iMaxPosOff - 1);
           }
       }
   }
   //
   // Since this place, every newpktpos is in the range
   // between m_iEndPos (inclusive) and a position for m_iMaxPosOff.

   // Here you can use prev_max_pos as the position represented
   // by m_iMaxPosOff, as if !extended_end, it was unchanged.
   else if (newpktpos == m_iEndPos)
   {
       // Case [D]: inserted a packet at the first gap following the
       // contiguous region. This makes a potential to extend the
       // contiguous region and we need to find its end.

       // If insertion happened at the very first packet, it is the
       // new earliest packet now. In any other situation under this
       // condition there's some contiguous packet range preceding
       // this position.
       if (m_iEndPos == m_iStartPos)
       {
           earlier_time = getPktTsbPdTime(unit->m_Packet.getMsgTimeStamp());
       }

       updateGapInfo(prev_max_pos);
   }
   // XXX Not sure if that's the best performant comparison
   // What is meant here is that newpktpos is between
   // m_iEndPos and m_iDropPos, though we know it's after m_iEndPos.
   // CONSIDER: make m_iDropPos rather m_iDropOff, this will make
   // this comparison a simple subtraction. Note that offset will
   // have to be updated on every shift of m_iStartPos.

   //else if (cmpPos(newpktpos, m_iDropPos) < 0)
   else if (newpktpos.cmp(m_iDropPos, m_iStartPos) < 0)
   {
       // Case [C]: the newly inserted packet precedes the
       // previous earliest delivery position after drop,
       // that is, there is now a "better" after-drop delivery
       // candidate.

       // New position updated a valid packet on an earlier
       // position than the drop position was before, although still
       // following a gap.
       //
       // We know it because if the position has filled a gap following
       // a valid packet, this preceding valid packet would be pointed
       // by m_iDropPos, or it would point to some earlier packet in a
       // contiguous series of valid packets following a gap, hence
       // the above condition wouldn't be satisfied.
       m_iDropPos = newpktpos;

       // If there's an inserted packet BEFORE drop-pos (which makes it
       // a new drop-pos), while the very first packet is absent (the
       // below condition), it means we have a new earliest-available
       // packet. Otherwise we would have only a newly updated drop
       // position, but still following some earlier contiguous range
       // of valid packets - so it's earlier than previous drop, but
       // not earlier than the earliest packet.
       if (m_iStartPos == m_iEndPos)
       {
           earlier_time = getPktTsbPdTime(unit->m_Packet.getMsgTimeStamp());
       }
   }
   // OTHERWISE: case [D] in which nothing is to be updated.

   return earlier_time;
}

void CRcvBuffer::updateGapInfo(CPos prev_max_pos)
{
    CPos pos = m_iEndPos;

    // First, search for the next gap, max until m_iMaxPosOff.
    for ( ; pos != prev_max_pos; ++pos /*pos = incPos(pos)*/)
    {
        if (m_entries[pos].status == EntryState_Empty)
        {
            break;
        }
    }
    if (pos == prev_max_pos)
    {
        // Reached the end and found no gaps.
        m_iEndPos = prev_max_pos;
        m_iDropPos = prev_max_pos;
    }
    else
    {
        // Found a gap at pos
        m_iEndPos = pos;
        m_iDropPos = pos; // fallback, although SHOULD be impossible
        // So, search for the first position to drop up to.
        for ( ; pos != prev_max_pos; ++pos /*pos = incPos(pos)*/)
        {
            if (m_entries[pos].status != EntryState_Empty)
            {
                m_iDropPos = pos;
                break;
            }
        }
    }
}

/// Request to remove from the receiver buffer
/// all packets with earlier sequence than @a seqno.
/// (Meaning, the packet with given sequence shall
/// be the first packet in the buffer after the operation).
int CRcvBuffer::dropUpTo(int32_t seqno)
{
    IF_RCVBUF_DEBUG(ScopedLog scoped_log);
    IF_RCVBUF_DEBUG(scoped_log.ss << "CRcvBuffer::dropUpTo: seqno " << seqno << " m_iStartSeqNo " << m_iStartSeqNo);

    COff len = COff(CSeqNo(seqno) - m_iStartSeqNo);
    //int len = CSeqNo::seqoff(m_iStartSeqNo, seqno);
    if (len <= 0)
    {
        IF_RCVBUF_DEBUG(scoped_log.ss << ". Nothing to drop.");
        return 0;
    }

    m_iMaxPosOff -= len;
    if (m_iMaxPosOff < 0)
        m_iMaxPosOff = 0;

    const int iDropCnt = len VALUE;
    while (len VALUE > 0)
    {
        dropUnitInPos(m_iStartPos);
        m_entries[m_iStartPos].status = EntryState_Empty;
        SRT_ASSERT(m_entries[m_iStartPos].pUnit == NULL && m_entries[m_iStartPos].status == EntryState_Empty);
        //m_iStartPos = incPos(m_iStartPos);
        ++m_iStartPos;
        --len;
    }

    // Update positions
    m_iStartSeqNo = CSeqNo(seqno);
    // Move forward if there are "read/drop" entries.
    // (This call MAY shift m_iStartSeqNo further.)
    releaseNextFillerEntries();

    // Start from here and search fort the next gap
    m_iEndPos = m_iDropPos = m_iStartPos;
    updateGapInfo(incPos(m_iStartPos, m_iMaxPosOff));
    //updateGapInfo(m_iStartPos + m_iMaxPosOff);

    // If the nonread position is now behind the starting position, set it to the starting position and update.
    // Preceding packets were likely missing, and the non read position can probably be moved further now.
    if (!isInUsedRange( m_iFirstNonreadPos))
    {
        m_iFirstNonreadPos = m_iStartPos;
        updateNonreadPos();
    }
    if (!m_tsbpd.isEnabled() && m_bMessageAPI)
        updateFirstReadableNonOrder();

    IF_HEAVY_LOGGING(debugShowState(("drop %" + Sprint(seqno)).c_str()));
    return iDropCnt;
}

int CRcvBuffer::dropAll()
{
    if (empty())
        return 0;

    //const int end_seqno = CSeqNo::incseq(m_iStartSeqNo, m_iMaxPosOff);
    const int end_seqno = (m_iStartSeqNo + m_iMaxPosOff VALUE) VALUE;
    return dropUpTo(end_seqno);
}

int CRcvBuffer::dropMessage(int32_t seqnolo, int32_t seqnohi, int32_t msgno, DropActionIfExists actionOnExisting)
{
    IF_RCVBUF_DEBUG(ScopedLog scoped_log);
    IF_RCVBUF_DEBUG(scoped_log.ss << "CRcvBuffer::dropMessage(): %(" << seqnolo << " - " << seqnohi << ")"
                                  << " #" << msgno << " actionOnExisting=" << actionOnExisting << " m_iStartSeqNo=%"
                                  << m_iStartSeqNo);

    // Drop by packet seqno range to also wipe those packets that do not exist in the buffer.
    //const int offset_a = CSeqNo::seqoff(m_iStartSeqNo, seqnolo);
    //const int offset_b = CSeqNo::seqoff(m_iStartSeqNo, seqnohi);
    const int offset_a = CSeqNo(seqnolo) - m_iStartSeqNo;
    const int offset_b = CSeqNo(seqnohi) - m_iStartSeqNo;
    if (offset_b < 0)
    {
        LOGC(rbuflog.Debug, log << "CRcvBuffer.dropMessage(): nothing to drop. Requested [" << seqnolo << "; "
            << seqnohi << "]. Buffer start " << m_iStartSeqNo VALUE << ".");
        return 0;
    }

    const bool bKeepExisting = (actionOnExisting == KEEP_EXISTING);
    COff minDroppedOffset = COff(-1);
    int iDropCnt = 0;
    const COff start_off = COff(max(0, offset_a));
    const CPos start_pos = incPos(m_iStartPos, start_off);
    //const CPos start_pos = m_iStartPos + start_off;
    const COff end_off = COff(min((int) m_szSize - 1, offset_b + 1));
    const CPos end_pos = incPos(m_iStartPos, end_off);
    //const CPos end_pos = m_iStartPos + end_off;
    bool bDropByMsgNo = msgno > SRT_MSGNO_CONTROL; // Excluding both SRT_MSGNO_NONE (-1) and SRT_MSGNO_CONTROL (0).
    for (CPos i = start_pos; i != end_pos; i = incPos(i)) //  ++i)
    {
        // Check if the unit was already dropped earlier.
        if (m_entries[i].status == EntryState_Drop)
            continue;

        if (m_entries[i].pUnit)
        {
            const PacketBoundary bnd = packetAt(i).getMsgBoundary();

            // Don't drop messages, if all its packets are already in the buffer.
            // TODO: Don't drop a several-packet message if all packets are in the buffer.
            if (bKeepExisting && bnd == PB_SOLO)
            {
                bDropByMsgNo = false; // Solo packet, don't search for the rest of the message.
                LOGC(rbuflog.Debug,
                     log << "CRcvBuffer::dropMessage(): Skipped dropping an existing SOLO packet %"
                         << packetAt(i).getSeqNo() << ".");
                continue;
            }

            const int32_t msgseq = packetAt(i).getMsgSeq(m_bPeerRexmitFlag);
            if (msgno > SRT_MSGNO_CONTROL && msgseq != msgno)
            {
                LOGC(rbuflog.Warn, log << "CRcvBuffer.dropMessage(): Packet seqno %" << packetAt(i).getSeqNo() << " has msgno " << msgseq << " differs from requested " << msgno);
            }

            if (bDropByMsgNo && bnd == PB_FIRST)
            {
                // First packet of the message is about to be dropped. That was the only reason to search for msgno.
                bDropByMsgNo = false;
            }
        }

        dropUnitInPos(i);
        ++iDropCnt;
        m_entries[i].status = EntryState_Drop;
        if (minDroppedOffset == -1)
            minDroppedOffset = offPos(m_iStartPos, i);
            //minDroppedOffset = i - m_iStartPos;
    }

    if (bDropByMsgNo)
    {
        // If msgno is specified, potentially not the whole message was dropped using seqno range.
        // The sender might have removed the first packets of the message, and thus @a seqnolo may point to a packet in the middle.
        // The sender should have the last packet of the message it is requesting to be dropped.
        // Therefore we don't search forward, but need to check earlier packets in the RCV buffer.
        // Try to drop by the message number in case the message starts earlier than @a seqnolo.
        const CPos stop_pos = decPos(m_iStartPos);
        //const CPos stop_pos = m_iStartPos - COff(1);
        for (CPos i = start_pos; i != stop_pos; --i)
        {
            // Can't drop if message number is not known.
            if (!m_entries[i].pUnit) // also dropped earlier.
                continue;

            const PacketBoundary bnd = packetAt(i).getMsgBoundary();
            const int32_t msgseq = packetAt(i).getMsgSeq(m_bPeerRexmitFlag);
            if (msgseq != msgno)
                break;

            if (bKeepExisting && bnd == PB_SOLO)
            {
                LOGC(rbuflog.Debug,
                     log << "CRcvBuffer::dropMessage(): Skipped dropping an existing SOLO message packet %"
                         << packetAt(i).getSeqNo() << ".");
                break;
            }

            ++iDropCnt;
            dropUnitInPos(i);
            m_entries[i].status = EntryState_Drop;
            // As the search goes backward, i is always earlier than minDroppedOffset.
            minDroppedOffset = offPos(m_iStartPos, i);
            //minDroppedOffset = i - m_iStartPos;

            // Break the loop if the start of the message has been found. No need to search further.
            if (bnd == PB_FIRST)
                break;
        }
        IF_RCVBUF_DEBUG(scoped_log.ss << " iDropCnt " << iDropCnt);
    }

    // Check if units before m_iFirstNonreadPos are dropped.
    const bool needUpdateNonreadPos = (minDroppedOffset != -1 && minDroppedOffset <= getRcvDataSize());
    releaseNextFillerEntries();

    // XXX TEST AND FIX
    // Start from the last updated start pos and search fort the next gap
    m_iEndPos = m_iDropPos = m_iStartPos;
    updateGapInfo(end_pos);
    IF_HEAVY_LOGGING(debugShowState(
                ("dropmsg off %" + Sprint(seqnolo) + " #" + Sprint(msgno)).c_str()));

    if (needUpdateNonreadPos)
    {
        m_iFirstNonreadPos = m_iStartPos;
        updateNonreadPos();
    }
    if (!m_tsbpd.isEnabled() && m_bMessageAPI)
    {
        if (!checkFirstReadableNonOrder())
            m_iFirstNonOrderMsgPos = CPos_TRAP;
        updateFirstReadableNonOrder();
    }

    IF_HEAVY_LOGGING(debugShowState(("dropmsg off %" + Sprint(seqnolo)).c_str()));
    return iDropCnt;
}

bool CRcvBuffer::getContiguousEnd(int32_t& w_seq) const
{
    if (m_iStartPos == m_iEndPos)
    {
        // Initial contiguous region empty (including empty buffer).
        HLOGC(rbuflog.Debug, log << "CONTIG: empty, give up base=%" << m_iStartSeqNo VALUE);
        w_seq = m_iStartSeqNo VALUE;
        return m_iMaxPosOff > 0;
    }

    COff end_off = offPos(m_iStartPos, m_iEndPos);
    //COff end_off = m_iEndPos - m_iStartPos;

    //w_seq = CSeqNo::incseq(m_iStartSeqNo, end_off);
    w_seq = (m_iStartSeqNo + end_off VALUE) VALUE;

    HLOGC(rbuflog.Debug, log << "CONTIG: endD=" << end_off VALUE
            << " maxD=" << m_iMaxPosOff VALUE
            << " base=%" << m_iStartSeqNo VALUE
            << " end=%" << w_seq);

    return (end_off < m_iMaxPosOff);
}

int CRcvBuffer::readMessage(char* data, size_t len, SRT_MSGCTRL* msgctrl, pair<int32_t, int32_t>* pw_seqrange)
{
    const bool canReadInOrder = hasReadableInorderPkts();
    if (!canReadInOrder && m_iFirstNonOrderMsgPos == CPos_TRAP)
    {
        LOGC(rbuflog.Warn, log << "CRcvBuffer.readMessage(): nothing to read. Ignored isRcvDataReady() result?");
        return 0;
    }

    const CPos readPos = canReadInOrder ? m_iStartPos : m_iFirstNonOrderMsgPos;
    const bool isReadingFromStart = (readPos == m_iStartPos); // Indicates if the m_iStartPos can be changed

    IF_RCVBUF_DEBUG(ScopedLog scoped_log);
    IF_RCVBUF_DEBUG(scoped_log.ss << "CRcvBuffer::readMessage. m_iStartSeqNo " << m_iStartSeqNo << " m_iStartPos " << m_iStartPos << " readPos " << readPos);

    size_t remain = len;
    char* dst = data;
    int    pkts_read = 0;
    int    bytes_extracted = 0; // The total number of bytes extracted from the buffer.

    int32_t out_seqlo = SRT_SEQNO_NONE;
    int32_t out_seqhi = SRT_SEQNO_NONE;

    for (CPos i = readPos;; ++i) //i = incPos(i))
    {
        SRT_ASSERT(m_entries[i].pUnit);
        if (!m_entries[i].pUnit)
        {
            LOGC(rbuflog.Error, log << "CRcvBuffer::readMessage(): null packet encountered.");
            break;
        }

        const CPacket& packet  = packetAt(i);
        const size_t   pktsize = packet.getLength();
        const int32_t pktseqno = packet.getSeqNo();

        if (out_seqlo == SRT_SEQNO_NONE)
            out_seqlo = pktseqno;

        out_seqhi = pktseqno;

        // unitsize can be zero
        const size_t unitsize = std::min(remain, pktsize);
        memcpy(dst, packet.m_pcData, unitsize);
        remain -= unitsize;
        dst += unitsize;

        ++pkts_read;
        bytes_extracted += (int) pktsize;

        if (m_tsbpd.isEnabled())
            updateTsbPdTimeBase(packet.getMsgTimeStamp());

        if (m_numNonOrderPackets && !packet.getMsgOrderFlag())
            --m_numNonOrderPackets;

        const bool pbLast  = packet.getMsgBoundary() & PB_LAST;
        if (msgctrl && (packet.getMsgBoundary() & PB_FIRST))
        {
            msgctrl->msgno  = packet.getMsgSeq(m_bPeerRexmitFlag);
        }
        if (msgctrl && pbLast)
        {
            msgctrl->srctime = count_microseconds(getPktTsbPdTime(packet.getMsgTimeStamp()).time_since_epoch());
        }
        if (msgctrl)
            msgctrl->pktseq = pktseqno;

        releaseUnitInPos(i);
        if (isReadingFromStart)
        {
            //m_iStartPos = i + COff(1); //incPos(i);
            m_iStartPos = incPos(i);
            --m_iMaxPosOff;

            // m_iEndPos and m_iDropPos should be
            // equal to m_iStartPos only if the buffer
            // is empty - but in this case the extraction will
            // not be done. Otherwise m_iEndPos should
            // point to the first empty cell, and m_iDropPos
            // point to the first busy cell after a gap, or
            // at worst be equal to m_iEndPos.

            // Therefore none of them should be updated
            // because they should be constantly updated
            // on an incoming packet, while this function
            // should not read further than to the first
            // empty cell at worst.

            SRT_ASSERT(m_iMaxPosOff >= 0);
            m_iStartSeqNo = CSeqNo(pktseqno) + 1;
        }
        else
        {
            // If out of order, only mark it read.
            m_entries[i].status = EntryState_Read;
        }

        if (pbLast)
        {
            if (readPos == m_iFirstNonOrderMsgPos)
                m_iFirstNonOrderMsgPos = CPos_TRAP;
            break;
        }
    }

    countBytes(-pkts_read, -bytes_extracted);

    releaseNextFillerEntries();

    if (!isInUsedRange( m_iFirstNonreadPos))
    {
        m_iFirstNonreadPos = m_iStartPos;
        //updateNonreadPos();
    }

    // Now that we have m_iStartPos potentially shifted, reinitialize
    // m_iEndPos and m_iDropPos.

    CPos pend_pos = incPos(m_iStartPos, m_iMaxPosOff);
    //CPos pend_pos = m_iStartPos + m_iMaxPosOff;

    // First check: is anything in the beginning
    if (m_entries[m_iStartPos].status == EntryState_Avail)
    {
        // If so, shift m_iEndPos up to the first nonexistent unit
        // XXX Try to optimize search by splitting into two loops if necessary.

        m_iEndPos = incPos(m_iStartPos);
        //m_iEndPos = m_iStartPos + COff(1);
        while (m_entries[m_iEndPos].status == EntryState_Avail)
        {
            m_iEndPos = incPos(m_iEndPos);
            //m_iEndPos = m_iEndPos + COff(1);
            if (m_iEndPos == pend_pos)
                break;
        }

        // If we had first packet available, then there's also no drop pos.
        m_iDropPos = m_iEndPos;
    }
    else
    {
        // If not, reset m_iEndPos and search for the first after-drop candidate.
        m_iEndPos = m_iStartPos;
        m_iDropPos = m_iEndPos;

        // The container could have become empty.
        // Stay here if so.
        if (m_iStartPos != pend_pos)
        {
            while (m_entries[m_iDropPos].status != EntryState_Avail)
            {
                m_iDropPos = incPos(m_iDropPos);
                //m_iDropPos = m_iDropPos + COff(1);
                if (m_iDropPos == pend_pos)
                {
                    // Nothing found - set drop pos equal to end pos,
                    // which means there's no drop
                    m_iDropPos = m_iEndPos;
                    break;
                }
            }
        }
    }


    if (!m_tsbpd.isEnabled())
        // We need updateFirstReadableNonOrder() here even if we are reading inorder,
        // incase readable inorder packets are all read out.
        updateFirstReadableNonOrder();

    const int bytes_read = int(dst - data);
    if (bytes_read < bytes_extracted)
    {
        LOGC(rbuflog.Error, log << "readMessage: small dst buffer, copied only " << bytes_read << "/" << bytes_extracted << " bytes.");
    }

    IF_RCVBUF_DEBUG(scoped_log.ss << " pldi64 " << *reinterpret_cast<uint64_t*>(data));

    if (pw_seqrange)
        *pw_seqrange = make_pair(out_seqlo, out_seqhi);

    IF_HEAVY_LOGGING(debugShowState("readmsg"));
    return bytes_read;
}

namespace {
    /// @brief Writes bytes to file stream.
    /// @param data pointer to data to write.
    /// @param len the number of bytes to write
    /// @param dst_offset ignored
    /// @param arg a void pointer to the fstream to write to.
    /// @return true on success, false on failure
    bool writeBytesToFile(char* data, int len, int dst_offset SRT_ATR_UNUSED, void* arg)
    {
        fstream* pofs = reinterpret_cast<fstream*>(arg);
        pofs->write(data, len);
        return !pofs->fail();
    }

    /// @brief Copies bytes to the destination buffer.
    /// @param data pointer to data to copy.
    /// @param len the number of bytes to copy
    /// @param dst_offset offset in destination buffer
    /// @param arg A pointer to the destination buffer
    /// @return true on success, false on failure
    bool copyBytesToBuf(char* data, int len, int dst_offset, void* arg)
    {
        char* dst = reinterpret_cast<char*>(arg) + dst_offset;
        memcpy(dst, data, len);
        return true;
    }
}

int CRcvBuffer::readBufferTo(int len, copy_to_dst_f funcCopyToDst, void* arg)
{
    CPos p = m_iStartPos;
    const CPos end_pos = m_iFirstNonreadPos;

    const bool bTsbPdEnabled = m_tsbpd.isEnabled();
    const steady_clock::time_point now = (bTsbPdEnabled ? steady_clock::now() : steady_clock::time_point());

    int rs = len;
    while ((p != end_pos) && (rs > 0))
    {
        if (!m_entries[p].pUnit)
        {
            //p = incPos(p); // WTF? Return abandons the loop anyway.
            //++p;
            LOGC(rbuflog.Error, log << "readBufferTo: IPE: NULL unit found in file transmission");
            return -1;
        }

        const srt::CPacket& pkt = packetAt(p);

        if (bTsbPdEnabled)
        {
            const steady_clock::time_point tsPlay = getPktTsbPdTime(pkt.getMsgTimeStamp());
            HLOGC(rbuflog.Debug,
                log << "readBuffer: check if time to play:"
                << " NOW=" << FormatTime(now)
                << " PKT TS=" << FormatTime(tsPlay));

            if ((tsPlay > now))
                break; /* too early for this unit, return whatever was copied */
        }

        const int pktlen = (int)pkt.getLength();
        const int remain_pktlen = pktlen - m_iNotch;
        const int unitsize = std::min(remain_pktlen, rs);

        if (!funcCopyToDst(pkt.m_pcData + m_iNotch, unitsize, len - rs, arg))
            break;

        if (rs >= remain_pktlen)
        {
            releaseUnitInPos(p);
            p = incPos(p);
            //++p;
            m_iNotch = 0;

            m_iStartPos = p;
            --m_iMaxPosOff;
            SRT_ASSERT(m_iMaxPosOff VALUE >= 0);
            //m_iStartSeqNo = CSeqNo::incseq(m_iStartSeqNo);
            ++m_iStartSeqNo;
        }
        else
            m_iNotch += rs;

        rs -= unitsize;
    }

    const int iBytesRead = len - rs;
    /* we removed acked bytes form receive buffer */
    countBytes(-1, -iBytesRead);

    // Update positions
    // Set nonread position to the starting position before updating,
    // because start position was increased, and preceding packets are invalid.
    if (!isInUsedRange( m_iFirstNonreadPos))
    {
        m_iFirstNonreadPos = m_iStartPos;
    }

    if (iBytesRead == 0)
    {
        LOGC(rbuflog.Error, log << "readBufferTo: 0 bytes read. m_iStartPos=" << m_iStartPos VALUE
                << ", m_iFirstNonreadPos=" << m_iFirstNonreadPos VALUE);
    }

    IF_HEAVY_LOGGING(debugShowState("readbuf"));
    return iBytesRead;
}

int CRcvBuffer::readBuffer(char* dst, int len)
{
    return readBufferTo(len, copyBytesToBuf, reinterpret_cast<void*>(dst));
}

int CRcvBuffer::readBufferToFile(fstream& ofs, int len)
{
    return readBufferTo(len, writeBytesToFile, reinterpret_cast<void*>(&ofs));
}

bool CRcvBuffer::hasAvailablePackets() const
{
    return hasReadableInorderPkts() || (m_numNonOrderPackets > 0 && m_iFirstNonOrderMsgPos != CPos_TRAP);
}

int CRcvBuffer::getRcvDataSize() const
{
    return offPos(m_iStartPos, m_iFirstNonreadPos) VALUE;
    //return (m_iFirstNonreadPos - m_iStartPos) VALUE;
}

int CRcvBuffer::getTimespan_ms() const
{
    if (!m_tsbpd.isEnabled())
        return 0;

    if (m_iMaxPosOff == 0)
        return 0;

    CPos lastpos = incPos(m_iStartPos, m_iMaxPosOff - 1);
    //CPos lastpos = m_iStartPos + (m_iMaxPosOff - COff(1));

    // Normally the last position should always be non empty
    // if TSBPD is enabled (reading out of order is not allowed).
    // However if decryption of the last packet fails, it may be dropped
    // from the buffer (AES-GCM), and the position will be empty.
    SRT_ASSERT(m_entries[lastpos].pUnit != NULL || m_entries[lastpos].status == EntryState_Drop);
    while (m_entries[lastpos].pUnit == NULL && lastpos != m_iStartPos)
    {
        lastpos = decPos(lastpos);
        //--lastpos;
    }

    if (m_entries[lastpos].pUnit == NULL)
        return 0;

    CPos startpos = m_iStartPos;
    while (m_entries[startpos].pUnit == NULL && startpos != lastpos)
    {
        startpos = incPos(startpos);
        //++startpos;
    }

    if (m_entries[startpos].pUnit == NULL)
        return 0;

    const steady_clock::time_point startstamp =
        getPktTsbPdTime(packetAt(startpos).getMsgTimeStamp());
    const steady_clock::time_point endstamp = getPktTsbPdTime(packetAt(lastpos).getMsgTimeStamp());
    if (endstamp < startstamp)
        return 0;

    // One millisecond is added as a duration of a packet in the buffer.
    // If there is only one packet in the buffer, one millisecond is returned.
    return static_cast<int>(count_milliseconds(endstamp - startstamp) + 1);
}

int CRcvBuffer::getRcvDataSize(int& bytes, int& timespan) const
{
    ScopedLock lck(m_BytesCountLock);
    bytes = m_iBytesCount;
    timespan = getTimespan_ms();
    return m_iPktsCount;
}

CRcvBuffer::PacketInfo CRcvBuffer::getFirstValidPacketInfo() const
{
    // Default: no packet available.
    PacketInfo pi = { SRT_SEQNO_NONE, false, time_point() };

    const CPacket* pkt = NULL;

    // Very first packet available with no gap.
    if (m_entries[m_iStartPos].status == EntryState_Avail)
    {
        SRT_ASSERT(m_entries[m_iStartPos].pUnit);
        pkt = &packetAt(m_iStartPos);
    }
    // If not, get the information from the drop
    else if (m_iDropPos != m_iEndPos)
    {
        SRT_ASSERT(m_entries[m_iDropPos].pUnit);
        pkt = &packetAt(m_iDropPos);
        pi.seq_gap = true; // Available, but after a drop.
    }
    else
    {
        // If none of them point to a valid packet,
        // there is no packet available;
        return pi;
    }

    pi.seqno = pkt->getSeqNo();
    pi.tsbpd_time = getPktTsbPdTime(pkt->getMsgTimeStamp());
    return pi;
}

std::pair<int, int> CRcvBuffer::getAvailablePacketsRange() const
{
    const COff nonread_off = offPos(m_iStartPos, m_iFirstNonreadPos);
    const int seqno_last = CSeqNo::incseq(m_iStartSeqNo VALUE, nonread_off VALUE);
    //const int nonread_off = (m_iFirstNonreadPos - m_iStartPos) VALUE;
    //const int seqno_last = (m_iStartSeqNo + nonread_off) VALUE;
    return std::pair<int, int>(m_iStartSeqNo VALUE, seqno_last);
}

bool CRcvBuffer::isRcvDataReady(time_point time_now) const
{
    const bool haveInorderPackets = hasReadableInorderPkts();
    if (!m_tsbpd.isEnabled())
    {
        if (haveInorderPackets)
            return true;

        SRT_ASSERT((!m_bMessageAPI && m_numNonOrderPackets == 0) || m_bMessageAPI);
        return (m_numNonOrderPackets > 0 && m_iFirstNonOrderMsgPos != CPos_TRAP);
    }

    if (!haveInorderPackets)
        return false;

    const PacketInfo info = getFirstValidPacketInfo();

    return info.tsbpd_time <= time_now;
}

CRcvBuffer::PacketInfo CRcvBuffer::getFirstReadablePacketInfo(time_point time_now) const
{
    const PacketInfo unreadableInfo    = {SRT_SEQNO_NONE, false, time_point()};
    const bool       hasInorderPackets = hasReadableInorderPkts();

    if (!m_tsbpd.isEnabled())
    {
        if (hasInorderPackets)
        {
            const CPacket&   packet = packetAt(m_iStartPos);
            const PacketInfo info   = {packet.getSeqNo(), false, time_point()};
            return info;
        }
        SRT_ASSERT((!m_bMessageAPI && m_numNonOrderPackets == 0) || m_bMessageAPI);
        if (m_iFirstNonOrderMsgPos != CPos_TRAP)
        {
            SRT_ASSERT(m_numNonOrderPackets > 0);
            const CPacket&   packet = packetAt(m_iFirstNonOrderMsgPos);
            const PacketInfo info   = {packet.getSeqNo(), true, time_point()};
            return info;
        }
        return unreadableInfo;
    }

    if (!hasInorderPackets)
        return unreadableInfo;

    const PacketInfo info = getFirstValidPacketInfo();

    if (info.tsbpd_time <= time_now)
        return info;
    else
        return unreadableInfo;
}

void CRcvBuffer::countBytes(int pkts, int bytes)
{
    ScopedLock lock(m_BytesCountLock);
    m_iBytesCount += bytes; // added or removed bytes from rcv buffer
    m_iPktsCount  += pkts;
    if (bytes > 0)          // Assuming one pkt when adding bytes
    {
        if (!m_uAvgPayloadSz)
            m_uAvgPayloadSz = bytes;
        else
            m_uAvgPayloadSz = avg_iir<100>(m_uAvgPayloadSz, (unsigned) bytes);
    }
}

void CRcvBuffer::releaseUnitInPos(CPos pos)
{
    CUnit* tmp = m_entries[pos].pUnit;
    m_entries[pos] = Entry(); // pUnit = NULL; status = Empty
    if (tmp != NULL)
        m_pUnitQueue->makeUnitFree(tmp);
}

bool CRcvBuffer::dropUnitInPos(CPos pos)
{
    if (!m_entries[pos].pUnit)
        return false;
    if (m_tsbpd.isEnabled())
    {
        updateTsbPdTimeBase(packetAt(pos).getMsgTimeStamp());
    }
    else if (m_bMessageAPI && !packetAt(pos).getMsgOrderFlag())
    {
        --m_numNonOrderPackets;
        if (pos == m_iFirstNonOrderMsgPos)
            m_iFirstNonOrderMsgPos = CPos_TRAP;
    }
    releaseUnitInPos(pos);
    return true;
}

void CRcvBuffer::releaseNextFillerEntries()
{
    CPos pos = m_iStartPos;
    while (m_entries[pos].status == EntryState_Read || m_entries[pos].status == EntryState_Drop)
    {
        //m_iStartSeqNo = CSeqNo::incseq(m_iStartSeqNo);
        ++m_iStartSeqNo;
        releaseUnitInPos(pos);
        //pos = incPos(pos);
        ++pos;
        m_iStartPos = pos;
        --m_iMaxPosOff;
        if (m_iMaxPosOff < 0)
            m_iMaxPosOff = 0;
    }
}

// TODO: Is this function complete? There are some comments left inside.
void CRcvBuffer::updateNonreadPos()
{
    if (m_iMaxPosOff == 0)
        return;

    const CPos end_pos = incPos(m_iStartPos, m_iMaxPosOff); // The empty position right after the last valid entry.
    //const CPos end_pos =  m_iStartPos + m_iMaxPosOff; // The empty position right after the last valid entry.

    CPos pos = m_iFirstNonreadPos;
    while (m_entries[pos].pUnit && m_entries[pos].status == EntryState_Avail)
    {
        if (m_bMessageAPI && (packetAt(pos).getMsgBoundary() & PB_FIRST) == 0)
            break;

        for (CPos i = pos; i != end_pos; ++i) // i = incPos(i))
        {
            if (!m_entries[i].pUnit || m_entries[pos].status != EntryState_Avail)
            {
                break;
            }

            // m_iFirstNonreadPos is moved to the first position BEHIND
            // the PB_LAST packet of the message. There's no guaratnee that
            // the cell at this position isn't empty.

            // Check PB_LAST only in message mode.
            if (!m_bMessageAPI || packetAt(i).getMsgBoundary() & PB_LAST)
            {
                m_iFirstNonreadPos = incPos(i);
                //m_iFirstNonreadPos = i + COff(1);
                break;
            }
        }

        if (pos == m_iFirstNonreadPos || !m_entries[m_iFirstNonreadPos].pUnit)
            break;

        pos = m_iFirstNonreadPos;
    }
}

CPos CRcvBuffer::findLastMessagePkt()
{
    for (CPos i = m_iStartPos; i != m_iFirstNonreadPos; ++i) //i = incPos(i))
    {
        SRT_ASSERT(m_entries[i].pUnit);

        if (packetAt(i).getMsgBoundary() & PB_LAST)
        {
            return i;
        }
    }

    return CPos_TRAP;
}

void CRcvBuffer::onInsertNonOrderPacket(CPos insertPos)
{
    if (m_numNonOrderPackets == 0)
        return;

    // If the following condition is true, there is already a packet,
    // that can be read out of order. We don't need to search for
    // another one. The search should be done when that packet is read out from the buffer.
    //
    // There might happen that the packet being added precedes the previously found one.
    // However, it is allowed to re bead out of order, so no need to update the position.
    if (m_iFirstNonOrderMsgPos != CPos_TRAP)
        return;

    // Just a sanity check. This function is called when a new packet is added.
    // So the should be unacknowledged packets.
    SRT_ASSERT(m_iMaxPosOff > 0);
    SRT_ASSERT(m_entries[insertPos].pUnit);
    const CPacket& pkt = packetAt(insertPos);
    const PacketBoundary boundary = pkt.getMsgBoundary();

    //if ((boundary & PB_FIRST) && (boundary & PB_LAST))
    //{
    //    // This packet can be read out of order
    //    m_iFirstNonOrderMsgPos = insertPos;
    //    return;
    //}

    const int msgNo = pkt.getMsgSeq(m_bPeerRexmitFlag);
    // First check last packet, because it is expected to be received last.
    const bool hasLast = (boundary & PB_LAST) || (scanNonOrderMessageRight(insertPos, msgNo) != CPos_TRAP);
    if (!hasLast)
        return;

    const CPos firstPktPos = (boundary & PB_FIRST)
        ? insertPos
        : scanNonOrderMessageLeft(insertPos, msgNo);
    if (firstPktPos == CPos_TRAP)
        return;

    m_iFirstNonOrderMsgPos = firstPktPos;
    return;
}

bool CRcvBuffer::checkFirstReadableNonOrder()
{
    if (m_numNonOrderPackets <= 0 || m_iFirstNonOrderMsgPos == CPos_TRAP || m_iMaxPosOff == COff(0))
        return false;

    const CPos endPos = incPos(m_iStartPos, m_iMaxPosOff);
    //const CPos endPos = m_iStartPos + m_iMaxPosOff;
    int msgno = -1;
    for (CPos pos = m_iFirstNonOrderMsgPos; pos != endPos; pos = incPos(pos)) //   ++pos)
    {
        if (!m_entries[pos].pUnit)
            return false;

        const CPacket& pkt = packetAt(pos);
        if (pkt.getMsgOrderFlag())
            return false;

        if (msgno == -1)
            msgno = pkt.getMsgSeq(m_bPeerRexmitFlag);
        else if (msgno != pkt.getMsgSeq(m_bPeerRexmitFlag))
            return false;

        if (pkt.getMsgBoundary() & PB_LAST)
            return true;
    }

    return false;
}

void CRcvBuffer::updateFirstReadableNonOrder()
{
    if (hasReadableInorderPkts() || m_numNonOrderPackets <= 0 || m_iFirstNonOrderMsgPos != CPos_TRAP)
        return;

    if (m_iMaxPosOff == 0)
        return;

    // TODO: unused variable outOfOrderPktsRemain?
    int outOfOrderPktsRemain = (int) m_numNonOrderPackets;

    // Search further packets to the right.
    // First check if there are packets to the right.
    //const int lastPos = (m_iStartPos + m_iMaxPosOff - 1) % m_szSize;
    //const CPos lastPos = m_iStartPos + m_iMaxPosOff - COff(1);
    const CPos lastPos = incPos(m_iStartPos, m_iMaxPosOff - 1);

    CPos posFirst = CPos_TRAP;
    CPos posLast = CPos_TRAP;
    int msgNo = -1;

    for (CPos pos = m_iStartPos; outOfOrderPktsRemain; ++pos) //pos = incPos(pos))
    {
        if (!m_entries[pos].pUnit)
        {
            posFirst = posLast = CPos_TRAP;
            msgNo = -1;
            continue;
        }

        const CPacket& pkt = packetAt(pos);

        if (pkt.getMsgOrderFlag())   // Skip in order packet
        {
            posFirst = posLast = CPos_TRAP;
            msgNo = -1;
            continue;
        }

        --outOfOrderPktsRemain;

        const PacketBoundary boundary = pkt.getMsgBoundary();
        if (boundary & PB_FIRST)
        {
            posFirst = pos;
            msgNo = pkt.getMsgSeq(m_bPeerRexmitFlag);
        }

        if (pkt.getMsgSeq(m_bPeerRexmitFlag) != msgNo)
        {
            posFirst = posLast = CPos_TRAP;
            msgNo = -1;
            continue;
        }

        if (boundary & PB_LAST)
        {
            m_iFirstNonOrderMsgPos = posFirst;
            return;
        }

        if (pos == lastPos)
            break;
    }

    return;
}

CPos CRcvBuffer::scanNonOrderMessageRight(const CPos startPos, int msgNo) const
{
    // Search further packets to the right.
    // First check if there are packets to the right.
    //const int lastPos = (m_iStartPos + m_iMaxPosOff - 1) % m_szSize;
    //const CPos lastPos = m_iStartPos + m_iMaxPosOff - COff(1);
    const CPos lastPos = incPos(m_iStartPos, m_iMaxPosOff - 1);
    if (startPos == lastPos)
        return CPos_TRAP;

    CPos pos = startPos;
    do
    {
        //pos = incPos(pos);
        ++pos;
        if (!m_entries[pos].pUnit)
            break;

        const CPacket& pkt = packetAt(pos);

        if (pkt.getMsgSeq(m_bPeerRexmitFlag) != msgNo)
        {
            LOGC(rbuflog.Error, log << "Missing PB_LAST packet for msgNo " << msgNo);
            return CPos_TRAP;
        }

        const PacketBoundary boundary = pkt.getMsgBoundary();
        if (boundary & PB_LAST)
            return pos;
    } while (pos != lastPos);

    return CPos_TRAP;
}

CPos CRcvBuffer::scanNonOrderMessageLeft(const CPos startPos, int msgNo) const
{
    // Search preceding packets to the left.
    // First check if there are packets to the left.
    if (startPos == m_iStartPos)
        return CPos_TRAP;

    CPos pos = startPos;
    do
    {
        //pos = decPos(pos);
        --pos;

        if (!m_entries[pos].pUnit)
            return CPos_TRAP;

        const CPacket& pkt = packetAt(pos);

        if (pkt.getMsgSeq(m_bPeerRexmitFlag) != msgNo)
        {
            LOGC(rbuflog.Error, log << "Missing PB_FIRST packet for msgNo " << msgNo);
            return CPos_TRAP;
        }

        const PacketBoundary boundary = pkt.getMsgBoundary();
        if (boundary & PB_FIRST)
            return pos;
    } while (pos != m_iStartPos);

    return CPos_TRAP;
}

bool CRcvBuffer::addRcvTsbPdDriftSample(uint32_t usTimestamp, const time_point& tsPktArrival, int usRTTSample)
{
    return m_tsbpd.addDriftSample(usTimestamp, tsPktArrival, usRTTSample);
}

void CRcvBuffer::setTsbPdMode(const steady_clock::time_point& timebase, bool wrap, duration delay)
{
    m_tsbpd.setTsbPdMode(timebase, wrap, delay);
}

void CRcvBuffer::applyGroupTime(const steady_clock::time_point& timebase,
    bool                            wrp,
    uint32_t                        delay,
    const steady_clock::duration& udrift)
{
    m_tsbpd.applyGroupTime(timebase, wrp, delay, udrift);
}

void CRcvBuffer::applyGroupDrift(const steady_clock::time_point& timebase,
    bool                            wrp,
    const steady_clock::duration& udrift)
{
    m_tsbpd.applyGroupDrift(timebase, wrp, udrift);
}

CRcvBuffer::time_point CRcvBuffer::getTsbPdTimeBase(uint32_t usPktTimestamp) const
{
    return m_tsbpd.getTsbPdTimeBase(usPktTimestamp);
}

void CRcvBuffer::updateTsbPdTimeBase(uint32_t usPktTimestamp)
{
    m_tsbpd.updateTsbPdTimeBase(usPktTimestamp);
}

string CRcvBuffer::strFullnessState(int32_t iFirstUnackSeqNo, const time_point& tsNow) const
{
    stringstream ss;

    ss << "iFirstUnackSeqNo=" << iFirstUnackSeqNo << " m_iStartSeqNo=" << m_iStartSeqNo VALUE
       << " m_iStartPos=" << m_iStartPos VALUE << " m_iMaxPosOff=" << m_iMaxPosOff VALUE << ". ";

    ss << "Space avail " << getAvailSize(iFirstUnackSeqNo) << "/" << m_szSize << " pkts. ";

    if (m_tsbpd.isEnabled() && m_iMaxPosOff > 0)
    {
        const PacketInfo nextValidPkt = getFirstValidPacketInfo();
        ss << "(TSBPD ready in ";
        if (!is_zero(nextValidPkt.tsbpd_time))
        {
            ss << count_milliseconds(nextValidPkt.tsbpd_time - tsNow) << "ms";
            const CPos iLastPos = incPos(m_iStartPos, m_iMaxPosOff - 1);
            //const CPos iLastPos = m_iStartPos + m_iMaxPosOff - COff(1);
            if (m_entries[iLastPos].pUnit)
            {
                ss << ", timespan ";
                const uint32_t usPktTimestamp = packetAt(iLastPos).getMsgTimeStamp();
                ss << count_milliseconds(m_tsbpd.getPktTsbPdTime(usPktTimestamp) - nextValidPkt.tsbpd_time);
                ss << " ms";
            }
        }
        else
        {
            ss << "n/a";
        }
        ss << "). ";
    }

    ss << SRT_SYNC_CLOCK_STR " drift " << getDrift() / 1000 << " ms.";
    return ss.str();
}

CRcvBuffer::time_point CRcvBuffer::getPktTsbPdTime(uint32_t usPktTimestamp) const
{
    return m_tsbpd.getPktTsbPdTime(usPktTimestamp);
}

/* Return moving average of acked data pkts, bytes, and timespan (ms) of the receive buffer */
int CRcvBuffer::getRcvAvgDataSize(int& bytes, int& timespan)
{
    // Average number of packets and timespan could be small,
    // so rounding is beneficial, while for the number of
    // bytes in the buffer is a higher value, so rounding can be omitted,
    // but probably better to round all three values.
    timespan = static_cast<int>(round((m_mavg.timespan_ms())));
    bytes = static_cast<int>(round((m_mavg.bytes())));
    return static_cast<int>(round(m_mavg.pkts()));
}

/* Update moving average of acked data pkts, bytes, and timespan (ms) of the receive buffer */
void CRcvBuffer::updRcvAvgDataSize(const steady_clock::time_point& now)
{
    if (!m_mavg.isTimeToUpdate(now))
        return;

    int       bytes = 0;
    int       timespan_ms = 0;
    const int pkts = getRcvDataSize(bytes, timespan_ms);
    m_mavg.update(now, pkts, bytes, timespan_ms);
}

int32_t CRcvBuffer::getFirstLossSeq(int32_t fromseq, int32_t* pw_end)
{
    //int offset = CSeqNo::seqoff(m_iStartSeqNo, fromseq);
    int offset_val = CSeqNo(fromseq) - m_iStartSeqNo;
    COff offset (offset_val);

    // Check if it's still inside the buffer
    if (offset_val < 0 || offset >= m_iMaxPosOff)
    {
        HLOGC(rbuflog.Debug, log << "getFirstLossSeq: offset=" << offset VALUE << " for %" << fromseq
                << " (with max=" << m_iMaxPosOff VALUE << ") - NO LOSS FOUND");
        return SRT_SEQNO_NONE;
    }

    // Start position
    CPos frompos = incPos(m_iStartPos, offset);
    //CPos frompos = m_iStartPos + offset;

    // Ok; likely we should stand at the m_iEndPos position.
    // If this given position is earlier than this, then
    // m_iEnd stands on the first loss, unless it's equal
    // to the position pointed by m_iMaxPosOff.

    CSeqNo ret_seq = CSeqNo(SRT_SEQNO_NONE);
    COff ret_off = m_iMaxPosOff;
    COff end_off = offPos(m_iStartPos, m_iEndPos);
    //COff end_off = m_iEndPos - m_iStartPos;
    if (offset < end_off)
    {
        // If m_iEndPos has such a value, then there are
        // no loss packets at all.
        if (end_off != m_iMaxPosOff)
        {
            //ret_seq = CSeqNo::incseq(m_iStartSeqNo VALUE, end_off VALUE);
            ret_seq = m_iStartSeqNo + end_off VALUE;
            ret_off = end_off;
        }
    }
    else
    {
        // Could be strange, but just as the caller wishes:
        // find the first loss since this point on
        // You can't rely on m_iEndPos, you are beyond that now.
        // So simply find the next hole.

        // REUSE offset as a control variable
        for (; offset < m_iMaxPosOff; ++offset)
        {
            const CPos pos = incPos(m_iStartPos, offset);
            //const CPos pos = m_iStartPos + offset;
            if (m_entries[pos].status == EntryState_Empty)
            {
                ret_off = offset;
                //ret_seq = CSeqNo::incseq(m_iStartSeqNo, offset);
                ret_seq = m_iStartSeqNo + offset VALUE;
                break;
            }
        }
    }

    // If found no loss, just return this value and do not
    // rewrite nor look for anything.

    // Also no need to search anything if only the beginning was
    // being looked for.
    if (ret_seq == CSeqNo(SRT_SEQNO_NONE) || !pw_end)
        return ret_seq VALUE;

    // We want also the end range, so continue from where you
    // stopped.

    // Start from ret_off + 1 because we know already that ret_off
    // points to an empty cell.
    for (COff off = ret_off + COff(1); off < m_iMaxPosOff; ++off)
    {
        const CPos pos = incPos(m_iStartPos, off);
        //const CPos pos = m_iStartPos + off;
        if (m_entries[pos].status != EntryState_Empty)
        {
            //*pw_end = CSeqNo::incseq(m_iStartSeqNo, (off - 1) VALUE);
            *pw_end = (m_iStartSeqNo + (off - COff(1)) VALUE) VALUE;
            return ret_seq VALUE;
        }
    }

    // Fallback - this should be impossible, so issue a log.
    LOGC(rbuflog.Error, log << "IPE: empty cell pos=" << frompos VALUE << " %"
            //<< CSeqNo::incseq(m_iStartSeqNo, ret_off)
            << (m_iStartSeqNo + ret_off VALUE) VALUE
            << " not followed by any valid cell");

    // Return this in the last resort - this could only be a situation when
    // a packet has somehow disappeared, but it contains empty cells up to the
    // end of buffer occupied range. This shouldn't be possible at all because
    // there must be a valid packet at least at the last occupied cell.
    return SRT_SEQNO_NONE;
}


} // namespace srt
