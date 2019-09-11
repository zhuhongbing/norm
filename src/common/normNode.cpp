#include "normNode.h"
#include "normSession.h"

#include "normEncoderMDP.h"
#include "normEncoderRS8.h"  // 8-bit Reed-Solomon encoder of RFC 5510
#include "normEncoderRS16.h"  // 16-bit Reed-Solomon encoder of RFC 5510

NormNode::NormNode(class NormSession& theSession, NormNodeId nodeId)
 : session(theSession), id(nodeId), reference_count(1),
   parent(NULL), right(NULL), left(NULL)
{
    
}

NormNode::~NormNode()
{
}

void NormNode::Retain()
{
    reference_count++;
}  // end NormNode::Retain()

void NormNode::Release()
{
    if (reference_count)
        reference_count--;
    else
        PLOG(PL_ERROR, "NormNode::Release() releasing non-retained node?!\n");
    if (0 == reference_count) delete this;   
}  // end NormNode::Release()


const NormNodeId& NormNode::LocalNodeId() const 
    {return session.LocalNodeId();}

NormNode::Accumulator::Accumulator()
 : msb(0), lsb(0)
{
}



NormCCNode::NormCCNode(class NormSession& theSession, NormNodeId nodeId)
 : NormNode(theSession, nodeId)
{
    
}

NormCCNode::~NormCCNode()
{
}


NormSenderNode::CmdBuffer::CmdBuffer()
 : length(0), next(NULL)
{
}

NormSenderNode::CmdBuffer::~CmdBuffer()
{
}

const double NormSenderNode::DEFAULT_NOMINAL_INTERVAL = 2*NormSession::DEFAULT_GRTT_ESTIMATE;
const double NormSenderNode::ACTIVITY_INTERVAL_MIN = 1.0;  // 1 second min activity timeout
        
NormSenderNode::NormSenderNode(class NormSession& theSession, NormNodeId nodeId)
 : NormNode(theSession, nodeId), instance_id(0), robust_factor(session.GetRxRobustFactor()),
   synchronized(false), sync_id(0),
   is_open(false), segment_size(0), fec_m(0), ndata(0), nparity(0), 
   repair_boundary(BLOCK_BOUNDARY), decoder(NULL), erasure_loc(NULL),
   retrieval_loc(NULL), retrieval_pool(NULL),
   cc_sequence(0), cc_enable(false), cc_feedback_needed(false), cc_rate(0.0), 
   rtt_confirmed(false), is_clr(false), is_plr(false),
   slow_start(true), send_rate(0.0), recv_rate(0.0), recv_rate_prev(0.0),
   nominal_packet_size(0), cmd_buffer_head(NULL), cmd_buffer_tail(NULL),
   cmd_buffer_pool(NULL), resync_count(0),
   nack_count(0), suppress_count(0), completion_count(0), failure_count(0)
{
    repair_boundary = session.ReceiverGetDefaultRepairBoundary();
    sync_policy = session.ReceiverGetDefaultSyncPolicy();
    default_nacking_mode = session.ReceiverGetDefaultNackingMode();
    unicast_nacks = session.ReceiverGetUnicastNacks();
    
    max_pending_range = session.GetRxCacheMax();
            
    repair_timer.SetListener(this, &NormSenderNode::OnRepairTimeout);
    repair_timer.SetInterval(0.0);
    repair_timer.SetRepeat(1);
    
    activity_timer.SetListener(this, &NormSenderNode::OnActivityTimeout);
    double activityInterval = 2*NormSession::DEFAULT_GRTT_ESTIMATE*session.GetTxRobustFactor();
    if (activityInterval < ACTIVITY_INTERVAL_MIN) activityInterval = ACTIVITY_INTERVAL_MIN;
    activity_timer.SetInterval(activityInterval);
    activity_timer.SetRepeat(robust_factor);
    
    cc_timer.SetListener(this, &NormSenderNode::OnCCTimeout);
    cc_timer.SetInterval(0.0);
    cc_timer.SetRepeat(1);
    
    ack_timer.SetListener(this, &NormSenderNode::OnAckTimeout);
    ack_timer.SetInterval(0.0);
    ack_timer.SetRepeat(0);
    
    grtt_send_time.tv_sec = 0;
    grtt_send_time.tv_usec = 0;
    grtt_quantized = NormQuantizeRtt(NormSession::DEFAULT_GRTT_ESTIMATE);
    grtt_estimate = NormUnquantizeRtt(grtt_quantized);
    gsize_quantized = NormQuantizeGroupSize(NormSession::DEFAULT_GSIZE_ESTIMATE);
    gsize_estimate = NormUnquantizeGroupSize(gsize_quantized);
    
    backoff_factor = NormSession::DEFAULT_BACKOFF_FACTOR;
    
    rtt_quantized = NormQuantizeRtt(NormSession::DEFAULT_GRTT_ESTIMATE);
    rtt_estimate = NormUnquantizeRtt(rtt_quantized);
    
    loss_estimator.SetLossEventWindow(NormSession::DEFAULT_GRTT_ESTIMATE);
    loss_estimator.SetIgnoreLoss(session.GetEcnIgnoreLoss());
    loss_estimator.SetTolerateLoss(session.GetCCTolerateLoss());
    
    prev_update_time.tv_sec = 0;
    prev_update_time.tv_usec = 0;   
}


NormSenderNode::~NormSenderNode()
{
    Close();
}

bool NormSenderNode::Open(UINT16 instanceId)
{
    instance_id = instanceId;
    if (!rx_table.Init(max_pending_range))
    {
        PLOG(PL_FATAL, "NormSenderNode::Open() rx_table init error\n");
        Close();
        return false;
    }
    if (!rx_pending_mask.Init(max_pending_range, 0x0000ffff))
    {
        PLOG(PL_FATAL, "NormSenderNode::Open() rx_pending_mask init error\n");
        Close();
        return false;
    }
    if (!rx_repair_mask.Init(max_pending_range, 0x0000ffff))
    {
        PLOG(PL_FATAL, "NormSenderNode::Open() rx_repair_mask init error\n");
        Close();
        return false;
    }    
    is_open = true;
    synchronized = false;
    resync_count = 0;  // reset resync_count 
    return true;
}  // end NormSenderNode::Open()

void NormSenderNode::Close()
{
    if (activity_timer.IsActive()) activity_timer.Deactivate();
    if (repair_timer.IsActive()) repair_timer.Deactivate();
    if (cc_timer.IsActive()) cc_timer.Deactivate();   
    FreeBuffers(); 
    
    // Delete any command buffers from cmd_buffer queue
    while (NULL != cmd_buffer_head)
    {
        CmdBuffer* buf = cmd_buffer_head;
        cmd_buffer_head = buf->GetNext();
        delete buf;
    }
    // Delete any command buffers from cmd_buffer pool
    while (NULL != cmd_buffer_pool)
    {
        CmdBuffer* buf = cmd_buffer_pool;
        cmd_buffer_pool = buf->GetNext();
        delete buf;
    }
    
    rx_repair_mask.Destroy();
    rx_pending_mask.Destroy();
    rx_table.Destroy();
    if (NULL != decoder) {
        delete decoder;
    }
    synchronized = false;
    is_open = false;
    
}  // end NormSenderNode::Close()

bool NormSenderNode::AllocateBuffers(UINT8  fecId,
                                     UINT16 fecInstanceId,
                                     UINT8  fecM,
                                     UINT16 segmentSize, 
                                     UINT16 numData, 
                                     UINT16 numParity)
{    
    ASSERT(IsOpen());
    // Calculate how much memory each buffered block will require
    UINT16 blockSize = numData + numParity;
    unsigned long maskSize = blockSize >> 3;
    if (0 != (blockSize & 0x07)) maskSize++;
    unsigned long blockStateSpace = sizeof(NormBlock) +  blockSize * sizeof(char*) + 2*maskSize;
    unsigned long bufferSpace = session.RemoteSenderBufferSize();

    // The "bufferFactor" weight determines the ratio of segment buffers (blockSegmentSpace) to
    // allocated NormBlock (blockStateSpace).  
    // If "bufferFactor = 1.0", this is equivalent to the old scheme, where every allocated
    // block can be fully buffered (numData segs) for decoding (no seeking required).  If 
    // "bufferFactor = 0.0", only a guarantee of at least "numParity" segments per block is 
    // enforced.  Note that "bufferFactor" values > 0.0 help reduce "seeking" for decoding, 
    // but reduce the number of blocks for which NORM can keep state.  Note this only comes 
    // into play when NORM would be "buffer constrained"
    // (TBD) perhaps we should keep state for more blocks than we can even buffer parity for ???
    //       (this would reduce requests for full block retransmissions when resource constrained)
    //       (this would correspond to "bufferFactor < 0.0"
    double bufferFactor = 0.0;  // (TBD) let app control "bufferFactor"???
    unsigned long segPerBlock = 
        (unsigned long) ((bufferFactor * (double)numData) +
                         ((1.0 - bufferFactor) * (double)numParity) + 0.5);
    if (segPerBlock > numData) segPerBlock = numData;
    // If there's no parity, no segment buffering for decoding is required at all!
    // (Thus, the full rxbuffer space can be used for block state)
    if (0 == numParity) segPerBlock = 0;
    unsigned long blockSegmentSpace = segPerBlock * (segmentSize + NormDataMsg::GetStreamPayloadHeaderLength());    
    unsigned long blockSpace = blockStateSpace + blockSegmentSpace;
    unsigned long numBlocks = bufferSpace / blockSpace;

    // Round numBlocks upward
    if (bufferSpace > (numBlocks*blockSpace)) numBlocks++;

    // Always have at least 2 blocks in the pool
    if (numBlocks < 2) numBlocks = 2;

    unsigned long numSegments = numBlocks * segPerBlock;

    if (!block_pool.Init(numBlocks, blockSize))
    {
        PLOG(PL_FATAL, "NormSenderNode::AllocateBuffers() block_pool init error\n");
        Close();
        return false;
    }
    
    // The extra byte of segments is used for marking segments
    // which are "start segments" for messages encapsulated in a NormStreamObject
    if (!segment_pool.Init(numSegments, segmentSize+NormDataMsg::GetStreamPayloadHeaderLength()))
    {
        PLOG(PL_FATAL, "NormSenderNode::AllocateBuffers() segment_pool init error\n");
        Close();
        return false;
    }
    
    // The "retrieval_pool" is used for FEC block decoding
    // These segments are temporarily used for "retrieved" source symbol segments
    // needed for block decoding  (new rx buffer mgmt scheme)
    if (!(retrieval_pool = new char*[numData]))
    {
        PLOG(PL_FATAL, "NormSenderNode::AllocateBuffers() new retrieval_pool error: %s\n", GetErrorString());
        Close();
        return false;          
    }
    memset(retrieval_pool, 0, numData*sizeof(char*));
    for (UINT16 i = 0; i < numData; i++)
    {
        // allocate segment with extra byte for stream flags ...
        char* s = new char[segmentSize+NormDataMsg::GetStreamPayloadHeaderLength()];
        if (NULL == s)
        {
            PLOG(PL_FATAL, "NormSenderNode::AllocateBuffers() new retrieval segment error: %s\n", GetErrorString());
            Close();
            return false;
        }   
        retrieval_pool[i] = s;
    }
    retrieval_index = 0;
    
    if (!(retrieval_loc = new unsigned int[numData]))
    {
        PLOG(PL_FATAL, "NormSenderNode::AllocateBuffers() retrieval_loc allocation error: %s\n", GetErrorString());
        Close();
        return false;   
    }
    
    if (NULL != decoder) delete decoder;
    
    if (0 != numParity)
    {
        switch (fecId)
        {
            case 2:
                if (8 == fecM)
                {
                    if (NULL == (decoder = new NormDecoderRS8))
                    {
                        PLOG(PL_FATAL, "NormSenderNode::AllocateBuffers() new NormDecoderRS8 error: %s\n", GetErrorString());
                        Close();
                        return false; 
                    }
                }
                else if (16 == fecM)
                {
                    if (NULL == (decoder = new NormDecoderRS16))
                    {
                        PLOG(PL_FATAL, "NormSenderNode::AllocateBuffers() new NormDecoderRS16 error: %s\n", GetErrorString());
                        Close();
                        return false; 
                    }
                }
                else
                {
                    PLOG(PL_FATAL, "NormSenderNode::AllocateBuffers() error: unsupported fecId=2 'm' value %d!\n", fecM);
                    Close();
                    return false;
                }
                break;
            case 5:
                if (NULL == (decoder = new NormDecoderRS8))
                {
                    PLOG(PL_FATAL, "NormSenderNode::AllocateBuffers() new NormDecoderRS8 error: %s\n", GetErrorString());
                    Close();
                    return false; 
                }
                break;
            case 129:
#ifdef ASSUME_MDP_FEC 
                if (NULL == (decoder = new NormDecoderMDP))
                {
                    PLOG(PL_FATAL, "NormSenderNode::AllocateBuffers() new NormDecoderMDP error: %s\n", GetErrorString());
                    Close();
                    return false; 
                }
#else
                if (0 == fecInstanceId)
                {
                    if (NULL == (decoder = new NormDecoderRS8))
                    {
                        PLOG(PL_FATAL, "NormSenderNode::AllocateBuffers() new NormDecoderRS8 error: %s\n", GetErrorString());
                        Close();
                        return false; 
                    }
                }
                else
                {
                    PLOG(PL_FATAL, "NormSenderNode::AllocateBuffers() error: unknown fecId=129 instanceId!\n");
                    Close();
                    return false;
                }
#endif // if/else ASSUME_MDP_FEC
                break;
            default:
                PLOG(PL_FATAL, "NormSenderNode::AllocateBuffers() error: unknown fecId>%d!\n", fecId);
                Close();
                return false;     
        }
        if (!decoder->Init(numData, numParity, segmentSize+NormDataMsg::GetStreamPayloadHeaderLength()))
        {
            PLOG(PL_FATAL, "NormSenderNode::AllocateBuffers() decoder init error\n");
            Close();
            return false; 
        }
        if (!(erasure_loc = new unsigned int[numParity]))
        {
            PLOG(PL_FATAL, "NormSenderNode::AllocateBuffers() erasure_loc allocation error: %s\n",  GetErrorString());
            Close();
            return false;   
        }
    } 
    else
    {
        decoder = NULL;
    }  // end if/else (0 != numParity)
    segment_size = segmentSize;
    nominal_packet_size = (double)segmentSize;
    fec_id = fecId;
    fec_m = fecM;
    ndata = numData;
    nparity = numParity;
    IncrementResyncCount();
    return true;
}  // end NormSenderNode::AllocateBuffers()

void NormSenderNode::FreeBuffers()
{
    if (erasure_loc)
    {
        delete[] erasure_loc;
        erasure_loc = NULL;
    }
    if (NULL != decoder)
    {
        decoder->Destroy();
        delete decoder;
        decoder = NULL;
    }
    if (retrieval_loc)
    {
        delete[] retrieval_loc;
        retrieval_loc = NULL;
    }
    if (retrieval_pool)
    {
        for (unsigned int i = 0; i < ndata; i++)
        {
            if (retrieval_pool[i]) 
            {
                delete[] retrieval_pool[i];   
                retrieval_pool[i] = NULL;
            }
        }   
        delete[] retrieval_pool;
        retrieval_pool = NULL;
    }
    
    NormObject* obj;
    while ((obj = rx_table.Find(rx_table.RangeLo()))) 
    {
        UINT16 objectId = obj->GetId();
        AbortObject(obj);
        // We do the following to remember which _objects_ were pending
        rx_pending_mask.Set(objectId);
    }
    segment_pool.Destroy();
    block_pool.Destroy();
    segment_size = ndata = nparity = 0; 
}  // end NormSenderNode::FreeBuffers()

unsigned long NormSenderNode::CurrentStreamBufferUsage() const
{
    unsigned long usage = 0;
    NormObjectTable::Iterator it(rx_table);
    NormObject* obj;
    while (NULL != (obj = it.GetNextObject()))
    {
        if (obj->IsStream())
            usage += static_cast<NormStreamObject*>(obj)->CurrentBufferUsage();
    }    
    return usage;
}  // end NormSenderNode::CurrentStreamBufferUsage()

unsigned long NormSenderNode::PeakStreamBufferUsage() const
{
    unsigned long usage = 0;
    NormObjectTable::Iterator it(rx_table);
    NormObject* obj;
    while (NULL != (obj = it.GetNextObject()))
    {
        if (obj->IsStream())
            usage += static_cast<NormStreamObject*>(obj)->PeakBufferUsage();
    }    
    return usage;
}  // end NormSenderNode::PeakStreamBufferUsage()

unsigned long NormSenderNode::StreamBufferOverunCount() const
{
    unsigned long count = 0;
    NormObjectTable::Iterator it(rx_table);
    NormObject* obj;
    while (NULL != (obj = it.GetNextObject()))
    {
        if (obj->IsStream())
            count += static_cast<NormStreamObject*>(obj)->BufferOverunCount();
    }    
    return count;
}  // end NormSenderNode::StreamBufferOverunCount()


bool NormSenderNode::ReadNextCmd(char* buffer, unsigned int* buflen)
{
    if (NULL == buflen) return false;  // (TBD) indicate error type
    if (NULL != cmd_buffer_head)
    {
        if ((NULL == buffer) ||
            (*buflen < cmd_buffer_head->GetContentLength()))
        {
            // User is just querying for content length size.
            *buflen = cmd_buffer_head->GetContentLength();
            return false;
        }   
        else
        {
            // a) remove cmd from cmd_buffer queue
            CmdBuffer* buf = cmd_buffer_head;
            cmd_buffer_head = buf->GetNext();
            if (NULL == cmd_buffer_head) 
                cmd_buffer_tail = NULL;
            // b) copy content
            *buflen = buf->GetContentLength();
            memcpy(buffer, buf->GetContent(), *buflen);
            // c) put cmd into cmd_buffer pool
            buf->Append(cmd_buffer_pool);
            cmd_buffer_pool = buf;
            return true;
        }
    }
    else 
    {
        // Tell user there is no cmd content to read  
        *buflen = 0;
        return false;
    }
}  // end NormSenderNode::ReadNextCmd()

void NormSenderNode::SetRobustFactor(int value)
{
    robust_factor = value;
    // activity timer depends upon robust_factor
    // (TBD) do a proper rescaling here instead?
    double activityInterval = 2*session.GetTxRobustFactor()*grtt_estimate;
    if (activityInterval < ACTIVITY_INTERVAL_MIN) activityInterval = ACTIVITY_INTERVAL_MIN;
    activity_timer.SetInterval(activityInterval);
    activity_timer.SetRepeat(robust_factor);
    if (activity_timer.IsActive()) activity_timer.Reschedule();
}  // end NormSenderNode::SetRobustFactor()

void NormSenderNode::UpdateGrttEstimate(UINT8 grttQuantized)
{
    grtt_quantized = grttQuantized;
    grtt_estimate = NormUnquantizeRtt(grttQuantized);
    PLOG(PL_DEBUG, "NormSenderNode::UpdateGrttEstimate() node>%lu sender>%lu new grtt: %lf sec\n",
            LocalNodeId(), GetId(), grtt_estimate);
    // activity timer depends upon sender's grtt estimate
    // (TBD) do a proper rescaling here instead?
    double activityInterval = 2*session.GetTxRobustFactor()*grtt_estimate;
    if (activityInterval < ACTIVITY_INTERVAL_MIN) activityInterval = ACTIVITY_INTERVAL_MIN;
    activity_timer.SetInterval(activityInterval);
    if (activity_timer.IsActive()) activity_timer.Reschedule();
    // (TBD) Scale/reschedule repair_timer and/or cc_timer???
    session.Notify(NormController::GRTT_UPDATED, this, (NormObject*)NULL);
}  // end NormSenderNode::UpdateGrttEstimate()


void NormSenderNode::HandleCommand(const struct timeval& currentTime, 
                                   const NormCmdMsg&     cmd)
{
    UINT8 grttQuantized = cmd.GetGrtt();
    if (grttQuantized != grtt_quantized) UpdateGrttEstimate(grttQuantized);
    UINT8 gsizeQuantized = cmd.GetGroupSize();
    if (gsizeQuantized != gsize_quantized)
    {
        gsize_quantized = gsizeQuantized;
        gsize_estimate = NormUnquantizeGroupSize(gsizeQuantized);
        PLOG(PL_DEBUG, "NormSenderNode::HandleCommand() node>%lu sender>%lu new group size:%lf\n",
                LocalNodeId(), GetId(), gsize_estimate);
    }
    backoff_factor = (double)cmd.GetBackoffFactor();
        
    NormCmdMsg::Flavor flavor = cmd.GetFlavor();
    switch (flavor)
    {
        case NormCmdMsg::SQUELCH:
        {
            const NormCmdSquelchMsg& squelch = (const NormCmdSquelchMsg&)cmd;
            if (!synchronized)
            {
                // Cache the remote sender's "fec_id" so we will 
                // build proper NACKs since we have no prior state
                fec_id = squelch.GetFecId();
                if (2 == fec_id)  // see comment in HandleObjectMessage() method on this
                    fec_m = 16;
                else
                    fec_m = 8;
            }
            else
            {
                // TBD - should we confirm the sender's FEC config here???
            }
            // 1) Sync to squelch (discards all objects prior to squelch objectId)
            NormObjectId objectId = squelch.GetObjectId();
			Sync(objectId);
            // 2) Prune stream object if applicable
            NormObject* obj = rx_table.Find(objectId);
            if ((NULL != obj) && (NormObject::STREAM == obj->GetType()))
            {
                NormBlockId blockId = squelch.GetFecBlockId(fec_m);
                static_cast<NormStreamObject*>(obj)->Prune(blockId, true);   
            }
            // 3) Discard any invalidated objects (those listed in the squelch)
            UINT16 objCount = squelch.GetInvalidObjectCount();
            for (UINT16 i = 0; i < objCount; i++)
            {
                NormObjectId objId = squelch.GetInvalidObjectId(i);
                obj = rx_table.Find(objId);
                if (NULL != obj) AbortObject(obj);
                rx_pending_mask.Unset(objId);
            }
            break;
        }
            
        case NormCmdMsg::ACK_REQ:
            // (TBD) handle ack requests (i.e. incl. app-defined ack requests)
            break;
            
        case NormCmdMsg::CC:
        {
            // TBD - do some duplicate detection here ?
            const NormCmdCCMsg& cc = (const NormCmdCCMsg&)cmd;
            grtt_recv_time = currentTime;
            cc.GetSendTime(grtt_send_time);
            cc_sequence = cc.GetCCSequence();
            NormCCRateExtension ext;
            bool hasCCRateExtension = false;
            while (cc.GetNextExtension(ext))
            {
                if (NormHeaderExtension::CC_RATE == ext.GetType())
                {
                    hasCCRateExtension = true;
                    cc_enable = true;
                    send_rate = NormUnquantizeRate(ext.GetSendRate());
                    // Are we in the cc_node_list?
                    UINT8 flags, rtt;
                    UINT16 loss;
                    if (cc.GetCCNode(LocalNodeId(), flags, rtt, loss))
                    {
                        if (rtt != rtt_quantized)
                        {
                            rtt_quantized = rtt;
                            rtt_estimate = NormUnquantizeRtt(rtt);
                            loss_estimator.SetLossEventWindow(rtt_estimate);
                        }
                        rtt_confirmed = true;
                        if (0 != (flags & NormCC::CLR))
                        {
                            is_clr = true;
                            is_plr = false;
                        }
                        else if (0 != (flags & NormCC::PLR))
                        {
                            is_clr = false;
                            is_plr = true;   
                        }
                        else
                        {
                            is_clr = is_plr = false;   
                        }
                    }
                    else
                    {
                        is_clr = is_plr = false;
                    }
                    double maxBackoff;
                    
                    if (is_clr || is_plr || !session.Address().IsMulticast())
                    {
                        // Respond immediately (i.e., no backoff, holdoff etc)
                        maxBackoff = 0.0;
                        if (cc_timer.IsActive()) cc_timer.Deactivate();
                        cc_timer.ResetRepeat(); // makes sure timer phase is correct
                        OnCCTimeout(cc_timer);
                        break;
                    }
                    else
                    {
                        if (cc_timer.IsActive()) break;
                        double backoffFactor = backoff_factor;
                        backoffFactor = MAX(backoffFactor, 4.0);
                        maxBackoff = grtt_estimate*backoffFactor;
                    }
                    double backoffTime = 
                        (maxBackoff > 0.0) ?
                            ExponentialRand(maxBackoff, gsize_estimate) : 0.0;
                    // Bias backoff timeout based on our rate 
                    double r;
                    double ccLoss = slow_start ? 0.0 : LossEstimate();
                    if (0.0 == ccLoss)
                    {
                        r = recv_rate / send_rate;
                        cc_rate = 2.0 * recv_rate;
                    }
                    else
                    {
                        double nominalSize = nominal_packet_size ? nominal_packet_size : segment_size;
                        cc_rate = NormSession::CalculateRate(nominalSize, rtt_estimate, ccLoss);
                        r = cc_rate / send_rate;
                        r = MIN(r, 0.9);
                        r = MAX(r, 0.5);
                        r = (r - 0.5) / 0.4;
                    }
                    //DMSG(0, "NormSenderNode::HandleCommand(CC) node>%lu bias:%lf recv_rate:%lf send_rate:%lf "
                    //      "grtt:%lf gsize:%lf\n",
                    //        LocalNodeId(), r, 8.0e-03*recv_rate, 8.0e-03*send_rate,
                    // 

                    backoffTime = 0.25 * r * maxBackoff + 0.75 * backoffTime;
                    cc_timer.SetInterval(backoffTime);
                    PLOG(PL_TRACE, "NormSenderNode::HandleCommand() node>%lu begin CC back-off: %lf sec)...\n",
                            LocalNodeId(), backoffTime);
                    session.ActivateTimer(cc_timer);
                    break;
                }  // end if (CC_RATE == ext.GetType())
            }  // end while (GetNextExtension())
            // Disable CC feedback if sender doesn't want it
            if (!hasCCRateExtension && cc_enable) cc_enable = false;
            break;
        }    
        case NormCmdMsg::FLUSH:
        {
            // (TBD) should we force synchronize if we're expected
            // to positively acknowledge the FLUSH
            const NormCmdFlushMsg& flush = (const NormCmdFlushMsg&)cmd;
            bool doAck = false;
            UINT16 nodeCount = flush.GetAckingNodeCount();
            NormNodeId localId = LocalNodeId();
            for (UINT16 i = 0; i < nodeCount; i++)
            {
                // (TBD) also ACK if NORM_NODE_ANY is listed???
                if (flush.GetAckingNodeId(i) == localId)
                {
                    doAck = true;
                    break;   
                }
            } 
            NormObjectId objectId = flush.GetObjectId();
            NormBlockId blockId = 0;
            NormSegmentId symbolId = 0;
            if (!synchronized)
            {
                // Cache the remote sender's "fec_id" so we will 
                // build proper NACKs since we have no prior state
                fec_id = flush.GetFecId();
                if (2 == fec_id)  // see comment in HandleObjectMessage() method on this
                    fec_m = 16;
                else
                    fec_m = 8;
            }
            else if (flush.GetFecId() == fec_id)
            {
                blockId = flush.GetFecBlockId(fec_m);
                symbolId = flush.GetFecSymbolId(fec_m);  
            }
            else
            {
                // TBD - should we confirm the sender's FEC config here???
            }
            if (!synchronized)
            {
                if (doAck)
                {
                    // Force sync since we're expected to ACK 
                    // and request repair for object indicated
                    Sync(objectId);
                }
                else
                {
                    // (TBD) optionally sync on any flush ?
                }            
            }
            if (synchronized)
            {
                if (doAck) // this was a watermark flush
                {
                    if (!PassiveRepairCheck(objectId, blockId, symbolId))
                    {
                       watermark_object_id = objectId;
                       watermark_block_id = blockId;  
                       watermark_segment_id = symbolId;
                       if (!ack_timer.IsActive())
                       {
                            double ackBackoff = session.Address().IsMulticast() ? UniformRand(grtt_estimate) : 0.0;
                            ack_timer.SetInterval(ackBackoff);
                            session.ActivateTimer(ack_timer); 
                       }
                       break;  // no pending repairs, skip regular "RepairCheck"   
                    }
                }
                UpdateSyncStatus(objectId);
                RepairCheck(NormObject::THRU_SEGMENT, objectId, blockId, symbolId);
            }
            break;
        }   
        case NormCmdMsg::REPAIR_ADV:
        {
            const NormCmdRepairAdvMsg& repairAdv = (const NormCmdRepairAdvMsg&)cmd;
            // Does the CC feedback of this ACK suppress our CC feedback?
            if (!is_clr && !is_plr && cc_timer.IsActive() && 
                cc_timer.GetRepeatCount())
            {
                NormCCFeedbackExtension ext;
                while (repairAdv.GetNextExtension(ext))
                {
                    if (NormHeaderExtension::CC_FEEDBACK == ext.GetType())
                    {
                        HandleCCFeedback(ext.GetCCFlags(), NormUnquantizeRate(ext.GetCCRate()));
                        break;
                    }
                }
            }   
            if (repair_timer.IsActive() && repair_timer.GetRepeatCount())
            {
                // (TBD) pay attention to the NORM_REPAIR_ADV_LIMIT flag
                HandleRepairContent(repairAdv.GetRepairContent(), 
                                    repairAdv.GetRepairContentLength());
            }
            break;
        }
        
           
        case NormCmdMsg::APPLICATION:
        {
            PLOG(PL_TRACE, "NormSenderNode::HandleCommand(APPLICATION) node>%lu recvd app-defined cmd...\n",
                            LocalNodeId());
            const NormCmdAppMsg& appCmd = static_cast<const NormCmdAppMsg&>(cmd);
            // 1) Buffer the received command either using a buffer structure
            //    cmd_buffer pool or allocating a new one as needed.
            CmdBuffer* buf = cmd_buffer_pool;
            if (NULL != buf) 
                cmd_buffer_pool = buf->GetNext();
            else
                buf = new CmdBuffer();
            if (NULL == buf)
            {
                PLOG(PL_ERROR, "NormSenderNode::HandleCommand(APPLICATION) node>%lu NewCmdCBuffer() error: %s\n", 
                        GetErrorString()); 
            }
            else 
            {
                unsigned int cmdLength = appCmd.GetContentLength();
                if ((cmdLength <= segment_size) ||
                    ((0 == segment_size) && (cmdLength < 8192)))
                {
                    // 2) Copy the app-defined command content into our buffer
                    buf->SetContent(appCmd.GetContent(), appCmd.GetContentLength());
                    // 3) Append the buffer into our cmd_buffer FIFO queue
                    if (NULL != cmd_buffer_tail)
                    {
                        cmd_buffer_tail->Append(buf);
                        cmd_buffer_tail = buf;
                    }
                    else
                    {
                        cmd_buffer_head = cmd_buffer_tail = buf;
                    }
                    session.Notify(NormController::RX_CMD_NEW, this, NULL);
                }
                else
                {
                    PLOG(PL_ERROR, "NormSenderNode::HandleCommand(APPLICATION) node>%lu error: "
                            "cmd content greater than sender's segment_size?!\n"); 
                    buf->Append(cmd_buffer_pool);
                    cmd_buffer_pool = buf;
                }
            }
            break;
        }
            
        default:
            PLOG(PL_ERROR, "NormSenderNode::HandleCommand() recv'd unimplemented command!\n");
            break;
    }  // end switch(flavor)
    
}  // end NormSenderNode::HandleCommand()

void NormSenderNode::HandleCCFeedback(UINT8 ccFlags, double ccRate)
{
    //ASSERT(cc_timer.IsActive() && cc_timer.GetRepeatCount());
    if (0 == (ccFlags & NormCC::CLR))
    {
        // We're suppressed by non-CLR receivers with no RTT confirmed
        // and/or lower rate
        double nominalSize = nominal_packet_size ? nominal_packet_size : segment_size;
        double ccLoss = slow_start ? 0.0 : LossEstimate();
        double localRate = (0.0 == ccLoss) ? 
                                (2.0*recv_rate) :
                                NormSession::CalculateRate(nominalSize,
                                                           rtt_estimate,
                                                           ccLoss);
        // This increases our chance of being suppressed
        // (but is it a good idea?)
        localRate = MAX(localRate, cc_rate);
        
        bool hasRtt = (0 != (ccFlags & NormCC::RTT));
        bool suppressed;
        
        if (rtt_confirmed)
        {
            // If we have confirmed our own RTT we
            // are suppressed by _any_ receivers with
            // lower rate than our own
            if (localRate > (0.9 * ccRate))
                suppressed = true;
            else
                suppressed = false;
        }
        else
        {
            // If we haven't confirmed our own RTT we
            // are suppressed by only by other
            // non-confirmed receivers
            if (hasRtt)
                suppressed = false;
            else if (localRate > (0.9 * ccRate))
                suppressed = true;
            else
                suppressed = false;
        }
        if (suppressed)
        {
            // This sets a holdoff timeout for cc feedback when suppressed
            double backoffFactor = backoff_factor;
            backoffFactor = MAX(backoffFactor, 4.0);     // always use at least 4 for cc purposes
            cc_timer.SetInterval(grtt_estimate*backoffFactor);
            if (cc_timer.IsActive())
                cc_timer.Reschedule();
            else
                session.ActivateTimer(cc_timer);
            cc_timer.DecrementRepeatCount();
        }
    }
}  // end NormSenderNode::HandleCCFeedback()

void NormSenderNode::HandleAckMessage(const NormAckMsg& ack)
{
    // Does the CC feedback of this ACK suppress our CC feedback
    if (!is_clr && !is_plr && cc_timer.IsActive() && cc_timer.GetRepeatCount())
    {
        NormCCFeedbackExtension ext;
        while (ack.GetNextExtension(ext))
        {
            if (NormHeaderExtension::CC_FEEDBACK == ext.GetType())
            {
                HandleCCFeedback(ext.GetCCFlags(), NormUnquantizeRate(ext.GetCCRate()));
                break;
            }
        }
    }    
}  // end NormSenderNode::HandleAckMessage()

void NormSenderNode::HandleNackMessage(const NormNackMsg& nack)
{
    // Does the CC feedback of this NACK suppress our CC feedback
    if (!is_clr && !is_plr && cc_timer.IsActive() && cc_timer.GetRepeatCount())
    {
        NormCCFeedbackExtension ext;
        while (nack.GetNextExtension(ext))
        {
            if (NormHeaderExtension::CC_FEEDBACK == ext.GetType())
            {
                HandleCCFeedback(ext.GetCCFlags(), NormUnquantizeRate(ext.GetCCRate()));
                break;
            }
        }
    }
    // Receivers also care about recvd NACKS for NACK suppression
    if (repair_timer.IsActive() && repair_timer.GetRepeatCount())
        HandleRepairContent(nack.GetRepairContent(), nack.GetRepairContentLength());
}  // end NormSenderNode::HandleNackMessage()

// Receivers use this method to process NACK content overheard from other 
// receivers or via NORM_CMD(REPAIR_ADV) messages received from the sender.  
// Such content can "suppress" pending NACKs
// (TBD) add provision to handle case when NORM_REPAIR_ADV_FLAG_LIMIT was set
void NormSenderNode::HandleRepairContent(const UINT32* buffer, UINT16 bufferLen)
{
    // Parse NACK and incorporate into repair state masks
    NormRepairRequest req;
    UINT16 requestLength = 0;
    bool freshObject = true;
    NormObjectId prevObjectId(0);
    NormObject* object = NULL;
    bool freshBlock = true;
    NormBlockId prevBlockId = 0;
    NormBlock* block = NULL;
    while (0 != (requestLength = req.Unpack(buffer, bufferLen)))
    {      
        // Point "buffer" to next request and adjust "bufferLen"
        buffer += (requestLength/4); 
        bufferLen -= requestLength;
        // Process request
        enum NormRequestLevel {SEGMENT, BLOCK, INFO, OBJECT};
        NormRepairRequest::Form requestForm = req.GetForm();
        NormRequestLevel requestLevel;
        if (req.FlagIsSet(NormRepairRequest::SEGMENT))
            requestLevel = SEGMENT;
        else if (req.FlagIsSet(NormRepairRequest::BLOCK))
            requestLevel = BLOCK;
        else if (req.FlagIsSet(NormRepairRequest::OBJECT))
            requestLevel = OBJECT;
        else
        {
            requestLevel = INFO;
            ASSERT(req.FlagIsSet(NormRepairRequest::INFO));
        }
        bool repairInfo = req.FlagIsSet(NormRepairRequest::INFO);

        NormRepairRequest::Iterator iterator(req, fec_id, fec_m);  // assumes constant "m"
        NormObjectId nextObjectId, lastObjectId;
        NormBlockId nextBlockId, lastBlockId;
        UINT16 nextBlockLen, lastBlockLen;
        NormSegmentId nextSegmentId, lastSegmentId;
        while (iterator.NextRepairItem(&nextObjectId, &nextBlockId, 
                                       &nextBlockLen, &nextSegmentId))
        {
            if (NormRepairRequest::RANGES == requestForm)
            {
                if (!iterator.NextRepairItem(&lastObjectId, &lastBlockId, 
                                             &lastBlockLen, &lastSegmentId))
                {
                    PLOG(PL_ERROR, "NormSenderNode::HandleRepairContent() node>%lu recvd incomplete RANGE request!\n",
                            LocalNodeId());
                    continue;  // (TBD) break/return instead???  
                }  
                // (TBD) test for valid range form/level
            }
            else
            {
                lastObjectId = nextObjectId;
                lastBlockId = nextBlockId;
                lastBlockLen = nextBlockLen;
                lastSegmentId = nextSegmentId;
            }   
            
            switch(requestLevel)
            {
                case INFO:
                {
                    while (nextObjectId <= lastObjectId)
                    {
                        NormObject* obj = rx_table.Find(nextObjectId);
                        if (obj) obj->SetRepairInfo();  
                        nextObjectId++;
                    }
                    break;
                }
                case OBJECT:
                {
                    UINT16 numBits = (UINT16)(lastObjectId - nextObjectId) + 1;
                    rx_repair_mask.SetBits(nextObjectId, numBits);
                    break;
                }
                case BLOCK:
                {
                    if (nextObjectId != prevObjectId) freshObject = true;
                    if (freshObject) 
                    {
                        object = rx_table.Find(nextObjectId);
                        prevObjectId = nextObjectId;
                    }
                    if (object) 
                    {
                        if (repairInfo) object->SetRepairInfo();
                        object->SetRepairs(nextBlockId, lastBlockId); 
                    } 
                    break;
                }
                case SEGMENT:
                {
                    if (nextObjectId != prevObjectId) freshObject = true;
                    if (freshObject) 
                    {
                        object = rx_table.Find(nextObjectId);
                        prevObjectId = nextObjectId;
                    }
                    if (object)
                    {
                        if (repairInfo) object->SetRepairInfo(); 
                        if (nextBlockId != prevBlockId) freshBlock = true;
                        if (freshBlock)
                        {
                            block = object->FindBlock(nextBlockId);
                            prevBlockId = nextBlockId;
                        }
                        if (block) 
                            block->SetRepairs(nextSegmentId,lastSegmentId);
                    }
                    break;
                }
            }  // end switch(requestLevel)
        }  // end while (iterator.NextRepairItem())
    }  // end while (nack.UnpackRepairRequest())
}  // end NormSenderNode::HandleRepairContent()


void NormSenderNode::CalculateGrttResponse(const struct timeval&    currentTime,
                                           struct timeval&          grttResponse) const
{
    grttResponse.tv_sec = grttResponse.tv_usec = 0;
    if (grtt_send_time.tv_sec || grtt_send_time.tv_usec)
    {
        // 1st - Get current time
        grttResponse = currentTime;    
        // 2nd - Calculate hold_time (current_time - recv_time)
        if (grttResponse.tv_usec < grtt_recv_time.tv_usec)
        {
            grttResponse.tv_sec = grttResponse.tv_sec - grtt_recv_time.tv_sec - 1;
            grttResponse.tv_usec = 1000000 - (grtt_recv_time.tv_usec - grttResponse.tv_usec);
        }
        else
        {
            grttResponse.tv_sec = grttResponse.tv_sec - grtt_recv_time.tv_sec;
            grttResponse.tv_usec = grttResponse.tv_usec - grtt_recv_time.tv_usec;
        }
        // 3rd - Calculate adjusted grtt_send_time (hold_time + send_time)
        grttResponse.tv_sec += grtt_send_time.tv_sec;
        grttResponse.tv_usec += grtt_send_time.tv_usec;
        if (grttResponse.tv_usec > 1000000)
        {
            grttResponse.tv_usec -= 1000000;
            grttResponse.tv_sec += 1;
        }    
    }
}  // end NormSenderNode::CalculateGrttResponse()

void NormSenderNode::DeleteObject(NormObject* obj)
{
    if (rx_table.Remove(obj))
    {
        rx_pending_mask.Unset(obj->GetId());
        obj->Close();
        obj->Release();
    }
}  // end NormSenderNode::DeleteObject()

NormBlock* NormSenderNode::GetFreeBlock(NormObjectId objectId, NormBlockId blockId)
{
    NormBlock* b = block_pool.Get();
    if (!b)
    {
        if (session.ReceiverIsSilent() || session.RcvrIsRealtime())
        {
            // forward iteration to find oldest older object with resources
            NormObjectTable::Iterator iterator(rx_table);
            NormObject* obj;
            while ((obj = iterator.GetNextObject()))
            {
                if (obj->GetId() > objectId)
                {
                    break;   
                }
                else
                {
                    if (obj->GetId() < objectId)
                        b = obj->StealOldestBlock(false);
                    else
                        b = obj->StealOldestBlock(true, blockId); 
                    if (b) 
                    {
                        b->EmptyToPool(segment_pool);
                        break;
                    }
                }
            }            
        }
        else
        {
            // reverse iteration to find newest newer object with resources
            NormObjectTable::Iterator iterator(rx_table);
            NormObject* obj;
            while ((obj = iterator.GetPrevObject()))
            {
                if (obj->GetId() < objectId) 
                {
                    break;
                }
                else
                {
                    if (obj->GetId() > objectId)
                        b = obj->StealNewestBlock(false); 
                    else 
                        b = obj->StealNewestBlock(true, blockId); 
                    if (b) 
                    {
                        b->EmptyToPool(segment_pool);
                        break;
                    }
                }
            } 
        }
    }  
    return b;
}  // end NormSenderNode::GetFreeBlock()

char* NormSenderNode::GetFreeSegment(NormObjectId objectId, NormBlockId blockId)
{
    if (segment_pool.IsEmpty())
    {
        // First, try to steal (retrievable) buffered source symbol segments
        NormObjectTable::Iterator iterator(rx_table);
        NormObject* obj;
        while ((obj = iterator.GetNextObject()))
        {
            // This takes source segments only from the "oldest" obj/blk
            // (TBD) Should these be from the "newest" obj/blk instead?
            if (obj->ReclaimSourceSegments(segment_pool))
                break;     
        }  
        // Second, if necessary, steal an ordinally "newer" block
        // (TBD) we might try to keep the block state, and only
        //       steal the segment needed?
        while (segment_pool.IsEmpty())
        {
            NormBlock* b = GetFreeBlock(objectId, blockId);
            if (b)
                block_pool.Put(b);
            else
                break;
        }
    }
    char* result = segment_pool.Get();
    return result;
}  // end NormSenderNode::GetFreeSegment()

void NormSenderNode::HandleObjectMessage(const NormObjectMsg& msg)
{
    UINT8 grttQuantized = msg.GetGrtt();
    if (grttQuantized != grtt_quantized) UpdateGrttEstimate(grttQuantized);
    UINT8 gsizeQuantized = msg.GetGroupSize();
    if (gsizeQuantized != gsize_quantized)
    {
        gsize_quantized = gsizeQuantized;
        gsize_estimate = NormUnquantizeGroupSize(gsizeQuantized);
        PLOG(PL_DEBUG, "NormSenderNode::HandleObjectMessage() node>%lu sender>%lu new group size: %lf\n",
                LocalNodeId(), GetId(), gsize_estimate);
    }
    backoff_factor = (double)msg.GetBackoffFactor();
    
    NormMsg::Type msgType = msg.GetType();
    NormObjectId objectId = msg.GetObjectId();
    UINT8 fecId = msg.GetFecId();
    // The current NORM implementation assumes senders maintain a fixed, common
    // set of FEC coding parameters for its transmissions.  The buffers (on a
    // "per-remote-sender basis") for receiver FEC processing are allocated here
    //  when:
    //    1) A NORM_DATA message is received and the buffers have not
    //       been previously allocated, or
    //    2) When the FEC parameters have changed (TBD)
    //
    bool allocateBuffers = true;
    if (BuffersAllocated())
    {
        if (fec_id == fecId)
            allocateBuffers = false;  // TBD - validate fec params?
        else
            FreeBuffers(); // force reallocation because fec id changed
    }
    
    NormBlockId blockId;
    NormSegmentId segmentId;
    if (NormMsg::INFO == msgType)
    {
        fec_id = fecId;
        // This assumes fec_id 2 (Reed Solomon) is only used when
        // 16-bit RS code is needed (otherwise fec_id 5 should have been used)
        // This is a limitation of the current RS specification where fec_m is
        // only provided in FEC OTI.  (Note we could dig into FEC OTI of NORM_INFO
        // message, but FLUSH/SQUELCH commands can't do this anyway and because we
        // defer buffer allocation until NORM_DATA is received, it doesn't really matter
        // (Any NACKs generate will have zero-value FEC blockId / symbolId anyway)
        if (2 == fecId)
            fec_m = 16;
        else
            fec_m = 8;
        blockId = 0;
        segmentId = 0;
    }
    else
    {
        if (allocateBuffers)
        {
            PLOG(PL_DEBUG, "NormSenderNode::HandleObjectMessage() node>%lu allocating sender>%lu buffers ...\n",
                    LocalNodeId(), GetId());
            // Currently,, our implementation requires the FEC Object Transmission Information
            // to properly allocate resources for FEC buffering and decoding
            UINT16 fecInstanceId = 0;
            UINT8 fecM = 8;
            UINT16 segmentSize = 0;
            UINT16 fecMaxBlockLen = 0;
            UINT16 fecNumParity = 0;
            bool noFTI = true;
            switch (fecId)
            {
                case 2:
                {
                    NormFtiExtension2 fti;
                    while (msg.GetNextExtension(fti))
                    {
                        if (NormHeaderExtension::FTI == fti.GetType())
                        {
                            ASSERT(1 == fti.GetFecGroupSize());  // TBD - allow for different groupings
                            fecM = fti.GetFecFieldSize();
                            segmentSize = fti.GetSegmentSize();
                            fecMaxBlockLen = fti.GetFecMaxBlockLen();
                            fecNumParity = fti.GetFecNumParity();
                            noFTI = false;
                            break;
                        }
                    }
                    break;
                }
                case 5:
                {
                    NormFtiExtension5 fti;
                    while (msg.GetNextExtension(fti))
                    {
                        if (NormHeaderExtension::FTI == fti.GetType())
                        {
                            segmentSize = fti.GetSegmentSize();
                            fecMaxBlockLen = fti.GetFecMaxBlockLen();
                            fecNumParity = fti.GetFecNumParity();
                            noFTI = false;
                            break;
                        }
                    }
                    break;
                }
                case 129:
                {
                    NormFtiExtension129 fti;
                    while (msg.GetNextExtension(fti))
                    {
                        if (NormHeaderExtension::FTI == fti.GetType())
                        {
                            fecInstanceId = fti.GetFecInstanceId();
                            segmentSize = fti.GetSegmentSize();
                            fecMaxBlockLen = fti.GetFecMaxBlockLen();
                            fecNumParity = fti.GetFecNumParity();
                            noFTI = false;
                            break;
                        }
                    }
                    break;
                }
                default:   
                    PLOG(PL_ERROR, "NormSenderNode::HandleObjectMessage() node>%lu sender>%lu unknown fec_id type:%d\n",
                                LocalNodeId(), GetId(), fecId);
                    return;
                
            }  // end switch (fecId)
            
            if (noFTI)
            {
                PLOG(PL_ERROR, "NormSenderNode::HandleObjectMessage() node>%lu sender>%lu - no FTI provided!\n",
                         LocalNodeId(), GetId());
                // (TBD) notify app of error ??
                return;  
            }
            if (!AllocateBuffers(fecId, fecInstanceId, fecM, segmentSize, fecMaxBlockLen, fecNumParity))
            {
                PLOG(PL_ERROR, "NormSenderNode::HandleObjectMessage() node>%lu sender>%lu buffer allocation error\n",
                        LocalNodeId(), GetId());
                // (TBD) notify app of error ??
                return;
            }    
        }  // end if (allocateBuffers)
                
        ASSERT(0 != fec_m);
        const NormDataMsg& data = static_cast<const NormDataMsg&>(msg);
        blockId = data.GetFecBlockId(fec_m);
        segmentId = data.GetFecSymbolId(fec_m);
    }  // end if/else (NormMsg::INFO == msgType)
    
    ObjectStatus status;
    if (synchronized)
    {
        status = UpdateSyncStatus(objectId);
    }
    else
    {      
        // Does this object message meet our sync policy?
        if (SyncTest(msg))
        {
            Sync(objectId);
            status = OBJ_NEW;
        }
        else
        {
            // The hacky use of "sync_id" here keeps the debug message from
            // printing too often while "waiting to sync" ...
            if (0 == sync_id)
            {
                PLOG(PL_ERROR, "NormSenderNode::HandleObjectMessage() waiting to sync ...\n");
                sync_id = 100;
            }
            else
            {
                sync_id--;
            }
            return;   
        }
    }    
    
    NormObject* obj = NULL;
    switch (status)
    {
        case OBJ_PENDING:
            if (NULL != (obj = rx_table.Find(objectId))) 
                break;
        case OBJ_NEW:
        {
            if (msg.FlagIsSet(NormObjectMsg::FLAG_STREAM))
            {
                if (!(obj = new NormStreamObject(session, this, objectId)))
                {
                    PLOG(PL_ERROR, "NormSenderNode::HandleObjectMessage() new NORM_OBJECT_STREAM error: %s\n",
                            GetErrorString());
                }
            }
            else if (msg.FlagIsSet(NormObjectMsg::FLAG_FILE))
            {
#ifdef SIMULATE
                if (!(obj = new NormSimObject(session, this, objectId)))
#else
                if (!(obj = new NormFileObject(session, this, objectId)))
#endif
                {
                    PLOG(PL_ERROR, "NormSenderNode::HandleObjectMessage() new NORM_OBJECT_FILE error: %s\n",
                            GetErrorString());
                }
            }
            else
            {
                if (!(obj = new NormDataObject(session, this, objectId)))
                {
                    PLOG(PL_ERROR, "NormSenderNode::HandleObjectMessage() new NORM_OBJECT_DATA error: %s\n",
                            GetErrorString());
                }
            }
            // TBD - if buffers were _just_ allocated above, we could avoid this second
            //       parsing of FTI header extension by promoting the FEC parameters learned
            //       above into stack variable that are still accessible here and adding a
            //       state variable to indicate they are valid
            if (NULL != obj)
            { 
                ASSERT(rx_table.CanInsert(objectId));
                ASSERT(rx_pending_mask.Test(objectId));
                rx_table.Insert(obj);
                UINT8 fecId = msg.GetFecId();
                UINT8 fecM = 8;
                UINT16 segmentSize = 0;
                UINT16 fecMaxBlockLen = 0;
                UINT16 fecNumParity = 0;
                NormObjectSize objectSize;
                bool noFTI = true;
                switch (fecId)
                {
                    case 2:
                    {
                        NormFtiExtension2 fti;
                        while (msg.GetNextExtension(fti))
                        {
                            if (NormHeaderExtension::FTI == fti.GetType())
                            {
                                ASSERT(1 == fti.GetFecGroupSize());  // TBD - allow for different groupings
                                fecM = fti.GetFecFieldSize();
                                segmentSize = fti.GetSegmentSize();
                                fecMaxBlockLen = fti.GetFecMaxBlockLen();
                                fecNumParity = fti.GetFecNumParity();
                                objectSize = fti.GetObjectSize();
                                noFTI = false;
                                break;
                            }
                        }
                        break;
                    }
                    case 5:
                    {
                        NormFtiExtension5 fti;
                        while (msg.GetNextExtension(fti))
                        {
                            if (NormHeaderExtension::FTI == fti.GetType())
                            {
                                segmentSize = fti.GetSegmentSize();
                                fecMaxBlockLen = fti.GetFecMaxBlockLen();
                                fecNumParity = fti.GetFecNumParity();
                                objectSize = fti.GetObjectSize();
                                noFTI = false;
                                break;
                            }
                        }
                        break;
                    }
                    case 129:
                    {
                        NormFtiExtension129 fti;
                        while (msg.GetNextExtension(fti))
                        {
                            if (NormHeaderExtension::FTI == fti.GetType())
                            {
                                segmentSize = fti.GetSegmentSize();
                                fecMaxBlockLen = fti.GetFecMaxBlockLen();
                                fecNumParity = fti.GetFecNumParity();
                                objectSize = fti.GetObjectSize();
                                noFTI = false;
                                break;
                            }
                        }
                        break;
                    }
                    default:   
                        segmentSize = fecMaxBlockLen = fecNumParity = 0;
                        PLOG(PL_ERROR, "NormSenderNode::HandleObjectMessage() node>%lu sender>%lu unknown fec_id type:%d\n",
                                    LocalNodeId(), GetId(), fecId);
                        return;

                }  // end switch (fecId)
                
                if (noFTI)
                {
                    PLOG(PL_ERROR, "NormSenderNode::HandleObjectMessage() node>%lu sender>%lu "
                                "new obj>%hu - no FTI provided!\n", LocalNodeId(), GetId(), (UINT16)objectId);
                    DeleteObject(obj);
                    obj = NULL;
                }
                else if (obj->RxOpen(objectSize, 
                                     msg.FlagIsSet(NormObjectMsg::FLAG_INFO),
                                     segmentSize, 
                                     fecId, 
                                     fecM,
                                     fecMaxBlockLen,
                                     fecNumParity))
                {
                    session.Notify(NormController::RX_OBJECT_NEW, this, obj);
                    if (obj->Accepted())
                    {
                        if (obj->IsStream()) 
                        {
                            // This initial "StreamUpdateStatus()" syncs the stream according to our sync policy
                            NormStreamObject* stream = static_cast<NormStreamObject*>(obj);
                            if (SYNC_CURRENT != sync_policy)
                                stream->StreamResync(NormBlockId(0));
                            else
                                stream->StreamUpdateStatus(blockId);
                        }    
                        PLOG(PL_DETAIL, "NormSenderNode::HandleObjectMessage() node>%lu sender>%lu new obj>%hu\n", 
                            LocalNodeId(), GetId(), (UINT16)objectId);
                    }
                    else
                    {
                        PLOG(PL_ERROR, "NormSenderNode::HandleObjectMessage() object not accepted\n");
                        DeleteObject(obj);
                        obj = NULL;    
                    }
                }
                else        
                {
                    PLOG(PL_ERROR, "NormSenderNode::HandleObjectMessage() error opening object\n");
                    DeleteObject(obj);
                    obj = NULL;   
                }
            }
            break;
        }
        case OBJ_COMPLETE:
            obj = NULL;
            break;
        default:
            ASSERT(0);
            break;
    }  // end switch(status)  
    
     
    
    if (obj)
    {
        obj->HandleObjectMessage(msg, msgType, blockId, segmentId);
        
        bool objIsPending = obj->IsPending();
        
        // Silent receivers may be configured to allow obj completion w/out INFO
        if (objIsPending && session.RcvrIgnoreInfo())
            objIsPending = obj->PendingMaskIsSet();
        
        if (!objIsPending)
        {
            // Reliable reception of this object has completed
            if (NormObject::FILE == obj->GetType()) 
#ifdef SIMULATE
                static_cast<NormSimObject*>(obj)->Close();           
#else
                static_cast<NormFileObject*>(obj)->Close();
#endif // !SIMULATE
            if (NormObject::STREAM != obj->GetType())
            {
                // Streams never complete unless they are "closed" by sender
                // and this is handled within stream control code in "normObject.cpp"
                session.Notify(NormController::RX_OBJECT_COMPLETED, this, obj);
                DeleteObject(obj);
                obj = NULL;
                completion_count++;
            }
        } 
    }  
    switch (repair_boundary)
    {
        case BLOCK_BOUNDARY:
            // Normal FEC "block boundary" repair check
            // (checks for repair needs for objects/blocks _prior_ to current objectId::blockId)
            RepairCheck(NormObject::TO_BLOCK, objectId, blockId, segmentId);
            break;
        case OBJECT_BOUNDARY:
            // Optional "object boundary repair check (non-streams only!)
            // (checks for repair needs for objects _prior_ to current objectId)
            // (also requests "info" for current objectId)
            if (obj && obj->IsStream())
                RepairCheck(NormObject::TO_BLOCK, objectId, blockId, segmentId);
            else
                RepairCheck(NormObject::THRU_INFO, objectId, blockId, segmentId);
            break;
    }
}  // end NormSenderNode::HandleObjectMessage()

bool NormSenderNode::SyncTest(const NormObjectMsg& msg) const
{
    switch (sync_policy)
    {
        case SYNC_CURRENT:  // default, more conservative "sync policy"
        case SYNC_STREAM:
	    {
            // Allow sync on stream at any time
            bool result = msg.FlagIsSet(NormObjectMsg::FLAG_STREAM);
            // Allow sync on INFO or block zero DATA message 
            result = result || (NormMsg::INFO == msg.GetType()) ? 
                                    true : (0 == ((const NormDataMsg&)msg).GetFecBlockId(fec_m));
            // Never sync on repair messages
            result = result && !msg.FlagIsSet(NormObjectMsg::FLAG_REPAIR);
            return result;
	    }
            
        case SYNC_ALL:     // sync on anything
            return true;
            
        default:
            ASSERT(0);  // should never occur
            return false;
    }
}  // end NormSenderNode::SyncTest()

// a little helper method
void NormSenderNode::AbortObject(NormObject* obj)
{
    // it it's a file, close it first, so app can do something
    if (NormObject::FILE == obj->GetType()) 
#ifdef SIMULATE
        static_cast<NormSimObject*>(obj)->Close();           
#else
        static_cast<NormFileObject*>(obj)->Close();
#endif // !SIMULATE
    session.Notify(NormController::RX_OBJECT_ABORTED, this, obj);
    DeleteObject(obj);
    failure_count++;
}  // end NormSenderNode::AbortObject()


// This method establishes the sync point "sync_id"
// objectId.  The sync point is the first ordinal
// object id for which the receiver is maintaining
// reliability.  Objects prior to the "sync point"
// are ignored.
// The related member variables and their purpose:
// "sync_id" - sync point object id, gets rolled upward
//             in NormSenderNode::SetPending() to deal with wrap
//
// "next_id" - id of next expected pending object
//             (set in NormSenderNode::SetPending())
//
// "max_pending_object" - max object id heard from sender
//             (inited in NormSenderNode::Sync() on 
//              initial sync, update in NormSenderNode::RepairCheck()
//
void NormSenderNode::Sync(NormObjectId objectId)
{
    if (synchronized)
    {
        NormObjectId firstPending;
        if (GetFirstPending(firstPending))
        {
            NormObjectId lastPending;
            GetLastPending(lastPending);
            if ((objectId > lastPending) || ((next_id - objectId) > max_pending_range))
            {
                NormObject* obj;
                while ((obj = rx_table.Find(rx_table.RangeLo()))) 
                {
                    AbortObject(obj);
                }
                rx_pending_mask.Clear(); 
                resync_count++;
            }
            else if (objectId > firstPending)
            {
               NormObject* obj;
               while ((obj = rx_table.Find(rx_table.RangeLo())) &&
                      (obj->GetId() < objectId)) 
               {
                   AbortObject(obj);
               }
               unsigned long numBits = (UINT16)(objectId - firstPending);
               rx_pending_mask.UnsetBits(firstPending, numBits); 
               resync_count++;
            }
        }  
        if ((next_id < objectId) || ((next_id - objectId) > max_pending_range))
        {
            max_pending_object = next_id = objectId;
        }
        sync_id = objectId;
        ASSERT(OBJ_INVALID != GetObjectStatus(objectId));
        if (OBJ_NEW == GetObjectStatus(objectId))
            SetPending(objectId);
    }
    else
    {
        ASSERT(!rx_pending_mask.IsSet());
        synchronized = true;
        switch (sync_policy)
        {
            case SYNC_CURRENT:  // this is the usual default
            case SYNC_STREAM:
                sync_id = next_id = max_pending_object = objectId;
                break;
            case SYNC_ALL:  // gratuitously sync for anything in our "range"
                sync_id = next_id = objectId - max_pending_range + 1;
                max_pending_object = objectId;
                break;
        }
        SetPending(objectId);  // inclusively sets pending mask for next_id..objectId
    }
}  // end NormSenderNode::Sync()

NormSenderNode::ObjectStatus NormSenderNode::UpdateSyncStatus(const NormObjectId& objectId)
{
    ASSERT(synchronized);
    ObjectStatus status = GetObjectStatus(objectId);
    switch (status)
    {
        case OBJ_INVALID:
        {
            // (TBD) We may want to control resync policy options
            //       or revert to fresh sync if sync is totally lost,
            //       otherwise SQUELCH process will get things in order
            PLOG(PL_INFO, "NormSenderNode::UpdateSyncStatus() node>%lu resync to sender>%lu obj>%hu...\n",
                    LocalNodeId(), GetId(), (UINT16)objectId);
            
            NormObjectId syncId = objectId;
            /*
            // This code avoids grosser resyncs (if uncommented) ...
            // However, attempts at finer-grained resync 
            // (i.e. preserving some partially-received objects)
            // has tended to exhibit an inability to ever "catch up"
            // But, note newer flow control feature might help here
            // so this might be worthwhile to some day uncomment
            // and experiment!
            if (rx_pending_mask.IsSet())
            {
                NormObjectId lastPending(65535);
                GetLastPending(lastPending);
                TRACE("lp:%hu\n", (UINT16)lastPending);
                if (syncId > lastPending)
                {
                    UINT16 delta = syncId - lastPending;
                    TRACE("SYNC DELTA = %d\n", delta);
                    if (delta < max_pending_range)
                    {
                        syncId -= (max_pending_range - 1);
                    }
                }
            }
            */
            Sync(syncId);
            //resync_count++;  (this is now incremented in NormNode::Sync())
            status = OBJ_NEW;
            break;
        }
        case OBJ_NEW:
            SetPending(objectId);
            break;
        default:
            break;
    }
    return status;
}  // end NormSenderNode::UpdateSyncStatus()

void NormSenderNode::SetPending(NormObjectId objectId)
{
    ASSERT(synchronized);
    ASSERT(OBJ_NEW == GetObjectStatus(objectId));
    if (objectId < next_id)
    {
        rx_pending_mask.Set(objectId);
    }
    else
    {
        UINT16 numBits = (UINT16)(objectId - next_id) + 1;
        rx_pending_mask.SetBits(next_id, numBits);
        next_id = objectId + 1; 
        // This prevents the "sync_id" from getting stale
        GetFirstPending(sync_id);
    }
}  // end NormSenderNode::SetPending()


NormSenderNode::ObjectStatus NormSenderNode::GetObjectStatus(const NormObjectId& objectId) const
{
   if (synchronized)
   {
       if (objectId < sync_id) 
       {
           // TBD - is there a better way this should be done?
           // If the object is a "little bit" old, it is probably an
           // object we recently completed.  If it is _very_ old,
           // we are probably "out of sync" with the sender?  Perhaps
           // this is too aggressive a resync rule?
           if ((sync_id - objectId) > 2*max_pending_range)
           {
                PLOG(PL_WARN, "NormSenderNode::GetObjectStatus() INVALID object>%hu sync_id>%hu\n", 
                              (UINT16)objectId, (UINT16)sync_id);
                return OBJ_INVALID;
           }
           else
           {
                return OBJ_COMPLETE;  
           }
       }
       else
       {
            if (objectId < next_id)
            {
                if (rx_pending_mask.Test(objectId))
                {
                    return OBJ_PENDING;
                }
                else
                {
                    return OBJ_COMPLETE;
                }
            }
            else
            {
                if (rx_pending_mask.IsSet())
                {
                    if (rx_pending_mask.CanSet(objectId))
                    {
                        ASSERT(rx_table.CanInsert(objectId));
                        return OBJ_NEW;
                    }
                    else
                    {
                        NormObjectId fp;
                        GetFirstPending(fp);
                        PLOG(PL_WARN, "NormSenderNode::GetObjectStatus() INVALID object>%hu firstPending>%hu\n", 
                              (UINT16)objectId, (UINT16)fp);
                        return OBJ_INVALID;
                    }
                }
                else
                {
                    NormObjectId delta = objectId - next_id + 1;
                    if (delta > NormObjectId((UINT16)rx_pending_mask.GetSize()))
                    {
                        PLOG(PL_WARN, "NormSenderNode::GetObjectStatus() INVALID object>%hu next_id>%hu\n", 
                              (UINT16)objectId, (UINT16)next_id);
                        
                        return OBJ_INVALID;
                    }
                    else
                    {
                        ASSERT(rx_table.CanInsert(objectId));
                        return OBJ_NEW;
                    }
                }
            }  
        }
   }
   else
   {
        return OBJ_NEW;   
   } 
}  // end NormSenderNode::GetObjectStatus()

// This is a "passive" THRU_SEGMENT repair check
// (used to for watermark ack check)
// Returns true if repairs are pending before and thru the given object:block:segment
bool NormSenderNode::PassiveRepairCheck(NormObjectId           objectId,  
                                        NormBlockId            blockId,
                                        NormSegmentId          segmentId)
{
    if (!synchronized) return true;
    NormObjectId nextId;
    if (GetFirstPending(nextId))
    {
        if (nextId < objectId)
        {
            return true;
        }
        else if (nextId == objectId)
        {
            NormObject* obj = rx_table.Find(nextId);
            if (NULL != obj)
                return obj->PassiveRepairCheck(blockId, segmentId);
            else
                return true;  // entire object pending
        }
        else
        {
            return false;     // it's an object already received (watermark past)
        }
    }
    else
    {
        return (OBJ_NEW == GetObjectStatus(objectId));
    }  
}  // end NormSenderNode::PassiveRepairCheck()

// This is the "active" repair check, which may activate NACKing
void NormSenderNode::RepairCheck(NormObject::CheckLevel checkLevel,
                                 NormObjectId           objectId,  
                                 NormBlockId            blockId,
                                 NormSegmentId          segmentId)
{    
    ASSERT(synchronized);
    if (objectId > max_pending_object) 
        max_pending_object = objectId;
    if (!repair_timer.IsActive())
    {
        // repair timer inactive
        bool startTimer = false;
        NormObjectId firstId;
        if (GetFirstPending(firstId))
        {
            NormObjectId nextId = firstId;
            do
            {
                if (nextId > objectId) break;
                NormObject* obj = rx_table.Find(nextId);
                if (NULL != obj)
                {
                    NormObject::CheckLevel level;
                    if (nextId < objectId)
                        level = NormObject::THRU_OBJECT;
                    else
                        level = checkLevel;
                    if (obj->ReceiverRepairCheck(level, blockId, segmentId, false))
                        startTimer = true;
                }
                else
                {
                    startTimer = true;
                }
                nextId++;
            } while (GetNextPending(nextId));
            current_object_id = objectId;
            if (startTimer)
            {
                // BACKOFF related code
                double backoffInterval = 
                    (session.Address().IsMulticast() && (backoff_factor > 0.0)) ?
                        ExponentialRand(grtt_estimate*backoff_factor, gsize_estimate) : 
                        0.0;
                PLOG(PL_DEBUG, "NormSenderNode::RepairCheck() node>%lu begin NACK backoff: %lf sec)...\n",
                        LocalNodeId(), backoffInterval);
                // Here, we clear NormSenderNode repair_mask
                // that is used for NACK suppression.
                // (object/block repair_masks are cleared as needed in NormObject::ReceiverRepairCheck
                if (rx_repair_mask.IsSet()) rx_repair_mask.Clear();
                repair_timer.SetInterval(backoffInterval);
                session.ActivateTimer(repair_timer);  
            }
        }
    }
    else if (repair_timer.GetRepeatCount())
    {
        // Repair timer in backoff phase
        // Trim sender current transmit position reference
        NormObject* obj = rx_table.Find(objectId);
        if (obj) obj->ReceiverRepairCheck(checkLevel, blockId, segmentId, true);
        if (objectId < current_object_id) current_object_id = objectId;
    }
    else
    {
        // Repair timer in holdoff phase   
        bool rewindDetected = false;
        if (objectId < current_object_id)
        {
            rewindDetected = true;
        }
        else if (objectId == current_object_id)
        {
            NormObject* obj = rx_table.Find(objectId);
            if (obj) 
                rewindDetected = obj->ReceiverRewindCheck(blockId, segmentId);
        }
        if (rewindDetected)
        {
            repair_timer.Deactivate();
            PLOG(PL_DEBUG, "NormSenderNode::RepairCheck() node>%lu sender rewind detected, ending NACK holdoff ...\n",
                    LocalNodeId());
            // Immediately do a repair check to see if rewind was sufficient
            // TBD - will we get too much unnecessary NACKing with out-of-order packet delivery ???
            RepairCheck(checkLevel, objectId, blockId, segmentId);
        }
    }
}  // end NormSenderNode::RepairCheck()


// When repair timer fires, possibly build a NACK
// and queue for transmission to this sender node
bool NormSenderNode::OnRepairTimeout(ProtoTimer& /*theTimer*/)
{
    switch(repair_timer.GetRepeatCount())
    {
        case 0:  // hold-off time complete
            PLOG(PL_DEBUG, "NormSenderNode::OnRepairTimeout() node>%lu end NACK hold-off ...\n",
                    LocalNodeId());
            break;
            
        case 1:  // back-off timeout complete
        {
            PLOG(PL_DEBUG, "NormSenderNode::OnRepairTimeout() node>%lu end NACK back-off ...\n",
                    LocalNodeId());
            // 1) Were we suppressed? 
            NormObjectId nextId;
            if (GetFirstPending(nextId))
            {
                // This loop checks to see if we have any repair pending objects
                // (If we don't have any, that means we were suppressed)
                bool repairPending = false;
                do
                {
                    if (nextId > current_object_id) break;
                    if (!rx_repair_mask.Test(nextId)) 
                    {
                        NormObject* obj = rx_table.Find(nextId);
                        if (!obj || obj->IsRepairPending(nextId != current_object_id))
                        {
                            repairPending = true;
                            break;
                        }                        
                    }
                    nextId++;
                } while (GetNextPending(nextId));
                
                if (repairPending)
                {
                    // We weren't completely suppressed, so build NACK
                    NormNackMsg* nack = (NormNackMsg*)session.GetMessageFromPool();
                    if (!nack)
                    {
                        PLOG(PL_WARN, "NormSenderNode::OnRepairTimeout() node>%lu Warning! "
                                "message pool empty ...\n", LocalNodeId());
                        repair_timer.Deactivate();
                        return false;   
                    }
                    nack->Init();
                    bool nackAppended = false;
                    
                    if (cc_enable)
                    {
                        NormCCFeedbackExtension ext;
                        nack->AttachExtension(ext);
                        if (is_clr) 
                            ext.SetCCFlag(NormCC::CLR);
                        else if (is_plr)
                            ext.SetCCFlag(NormCC::PLR);
                        if (rtt_confirmed)
                            ext.SetCCFlag(NormCC::RTT);
                        ext.SetCCRtt(rtt_quantized);
                        double ccLoss = slow_start ? 0.0 : LossEstimate();
                        //UINT16 lossQuantized = NormQuantizeLoss(ccLoss);
                        //ext.SetCCLoss(lossQuantized);
                        UINT16 lossQuantized = NormQuantizeLoss32(ccLoss);
                        ext.SetCCLoss32(lossQuantized);
                        //if (0.0 == ccLoss)
                        if (0 == lossQuantized)
                        {
                            //if (slow_start)   // (TBD) should we only set flag on actual slow_start?
                                ext.SetCCFlag(NormCC::START);
                            if (recv_rate > 0.0)
                                ext.SetCCRate(NormQuantizeRate(2.0 * recv_rate)); 
                            else
                                ext.SetCCRate(NormQuantizeRate(2.0 * nominal_packet_size));  // (TBD revisit this) 
                        }
                        else
                        {
                            double nominalSize = (nominal_packet_size > segment_size) ? nominal_packet_size : segment_size;
                            double ccRate = NormSession::CalculateRate(nominalSize,
                                                                       rtt_estimate,
                                                                       ccLoss);
#ifdef LIMIT_CC_RATE                     
                            // Experimental modification to NORM-CC where congestion control rate is limited
                            // to MIN(2.0*measured recv rate, calculated rate).  This might prevent large rate
                            // overshoot in conditions where the loss measurement (perhaps initial loss) is 
                            // very low due to big network packet buffers, etc   
                            double rxRate = 2.0*recv_rate;
                            if (rxRate < ccRate)
                            {
                                ext.SetCCFlag(NormCC::LIMIT);
                                ccRate = rxRate;
                            }
#endif // LIMIT_CC_RATE
                            ext.SetCCRate(NormQuantizeRate(ccRate));
                        }
                        PLOG(PL_DEBUG, "NormSenderNode::OnRepairTimeout() node>%lu sending NACK rate:%lf kbps (rtt:%lf loss:%lf s:%hu) slow_start:%d\n",
                                 LocalNodeId(), 8.0e-03*NormUnquantizeRate(ext.GetCCRate()), 
                                 rtt_estimate, ccLoss, (UINT16)nominal_packet_size, slow_start);
                        ext.SetCCSequence(cc_sequence);
                        // Cancel potential pending NORM_ACK(CC) since we are NACKing 
                        if (cc_timer.IsActive())
                        {
                            // Set holdoff timeout to refrain from sending too much cc feedback
                            cc_timer.SetInterval(grtt_estimate*backoff_factor);
                            cc_timer.Reschedule();
                            cc_timer.DecrementRepeatCount();   
                        }
                    }  // end if (cc_enable)
                    
                    // Iterate through rx pending object list, 
                    // appending repair requests as needed
                    NormRepairRequest req;
                    NormRepairRequest::Form prevForm = NormRepairRequest::INVALID;
                    bool iterating = GetFirstPending(nextId);
                    iterating = iterating && (nextId <= max_pending_object);
                    NormObjectId prevId = nextId;
                    UINT16 consecutiveCount = 0;
                    while (iterating || (0 != consecutiveCount))
                    {
                        bool appendRequest = false;
                        NormObject* obj = iterating ? rx_table.Find(nextId) : NULL;
                        if (obj)
                            appendRequest = true;
                        else if (iterating && ((nextId - prevId) == consecutiveCount))
                            consecutiveCount++;  // consecutive series of missing objs starts/continues 
                        else
                            appendRequest = true;  // consecutive series broken or finished
                        
                        if (appendRequest)
                        {
                            NormRepairRequest::Form nextForm;
                            switch (consecutiveCount)
                            {
                                case 0:
                                    nextForm = NormRepairRequest::INVALID;
                                    break;
                                case 1:
                                case 2:
                                    nextForm = NormRepairRequest::ITEMS;
                                    break;
                                default:
                                    nextForm = NormRepairRequest::RANGES;
                                    break;
                            }  
                            if (prevForm != nextForm)
                            {
                                if ((NormRepairRequest::INVALID != prevForm) &&
                                    (NormObject::NACK_NONE != default_nacking_mode))
                                {
                                    if (0 == nack->PackRepairRequest(req))
                                    {
                                        PLOG(PL_WARN, "NormSenderNode::OnRepairTimeout() warning: full NACK msg\n");
                                        break;
                                    } 
                                    nackAppended = true;
                                }
                                if (NormRepairRequest::INVALID != nextForm)
                                {
                                    if (0 != segment_size)
                                        nack->AttachRepairRequest(req, segment_size); // (TBD) error check
                                    else
                                        nack->AttachRepairRequest(req, NormNackMsg::DEFAULT_LENGTH_MAX);
                                    req.SetForm(nextForm);
                                    req.ResetFlags();
                                    // Set flags for missing objects according to
                                    // default "nacking mode"
                                    if (NormObject::NACK_INFO_ONLY == default_nacking_mode)
                                        req.SetFlag(NormRepairRequest::INFO);
                                    else
                                        req.SetFlag(NormRepairRequest::OBJECT);
                                }
                                prevForm = nextForm;
                            }
                            switch (nextForm)
                            {
                                case NormRepairRequest::ITEMS:
                                    req.AppendRepairItem(fec_id, fec_m, prevId, 0, ndata, 0);
                                    if (2 == consecutiveCount)
                                        req.AppendRepairItem(fec_id, fec_m, prevId+1, 0, ndata, 0);
                                    break;
                                case NormRepairRequest::RANGES:
                                    req.AppendRepairRange(fec_id, fec_m, prevId, 0, ndata, 0,
                                                          prevId+consecutiveCount-1, 0, ndata, 0);
                                    break;
                                default:
                                    break;  
                            }
                            
                            if (obj)
                            {
                                if (obj->IsPending(nextId != max_pending_object))
                                {
                                    if ((NormRepairRequest::INVALID != prevForm) &&
                                        (NormObject::NACK_NONE != default_nacking_mode))
                                    {
                                        if (0 == nack->PackRepairRequest(req))
                                        {
                                            PLOG(PL_WARN, "NormSenderNode::OnRepairTimeout() warning: full NACK msg\n");
                                            break;
                                        } 
                                        nackAppended = true;
                                    }
                                    prevForm = NormRepairRequest::INVALID; 
                                    bool flush = (nextId != max_pending_object);
                                    nackAppended |= obj->AppendRepairRequest(*nack, flush); 
                                }
                                consecutiveCount = 0;
                            }
                            else if (iterating)
                            {
                                consecutiveCount = 1;
                            }
                            else
                            {
                                consecutiveCount = 0;  // we're all done
                            }
                            prevId = nextId;
                        }  // end if (appendRequest)
                        nextId++;
                        iterating = GetNextPending(nextId);
                    } // end while (iterating || (0 != consecutiveCount))
                    // Pack in repair request "req" if it's outstanding
                    if ((NormRepairRequest::INVALID != prevForm) &&
                        (NormObject::NACK_NONE != default_nacking_mode))
                    {
                        if (0 != nack->PackRepairRequest(req))
                            nackAppended = true;
                        else
                            PLOG(PL_WARN, "NormSenderNode::OnRepairTimeout() warning: full NACK msg\n");
                    }
                    // Queue NACK for transmission
                    nack->SetSenderId(GetId());
                    nack->SetInstanceId(instance_id);
                    // GRTT response is deferred until transmit time
                    if (unicast_nacks)
                        nack->SetDestination(GetAddress());
                    else
                        nack->SetDestination(session.Address());
                    if (nackAppended)
                    {
                        // Debug check to make sure NACK has content
                        ASSERT(nack->GetRepairContentLength() > 0);
                        if (!session.ReceiverIsSilent())
                        {
                            session.SendMessage(*nack);
                            nack_count++;
                        }
                        session.ReturnMessageToPool(nack);
                    }
                    else
                    {
                        // The nack had no repair request content,
                        // perhaps because of our "nacking mode"
                        // even though there were pending objects
                        PLOG(PL_DEBUG, "NormSenderNode::OnRepairTimeout() node>%lu zero content nack ...\n",
                            LocalNodeId());
                        session.ReturnMessageToPool(nack);
                    }
                }
                else
                {
                    if (!session.ReceiverIsSilent())
                    {
                        suppress_count++;
                        PLOG(PL_DEBUG, "NormSenderNode::OnRepairTimeout() node>%lu NACK SUPPRESSED ...\n",
                                LocalNodeId());
                    }
                }  // end if/else(repairPending)
                // BACKOFF related code
                double holdoffInterval = grtt_estimate;
                if (session.Address().IsMulticast())
                {
                    holdoffInterval *= (backoff_factor + 2.0);
                }
                else
                {
                    // Allow at least a packet interval of "slop" time for holdoff
                    if (0.0 != recv_rate)
                    {
                        double nominalPktInterval = nominal_packet_size / recv_rate;
                        holdoffInterval += MIN(nominalPktInterval, grtt_estimate);
                    }
                    else
                    {
                        holdoffInterval += grtt_estimate;
                    }
                }
                //holdoffInterval = (backoff_factor > 0.0) ? holdoffInterval : 1.0*grtt_estimate;
                repair_timer.SetInterval(holdoffInterval);
                PLOG(PL_DEBUG, "NormSenderNode::OnRepairTimeout() node>%lu begin NACK hold-off: %lf sec ...\n",
                         LocalNodeId(), holdoffInterval);
                
            }
            else
            {
                PLOG(PL_DEBUG, "NormSenderNode::OnRepairTimeout() node>%lu nothing pending ...\n",
                        LocalNodeId());
                // (TBD) cancel hold-off timeout ???  
            }  // end if/else (repair_mask.IsSet())       
        }
        break;
        
        default: // should never occur
            ASSERT(0);
            break;
    }
    return true;
}  // end NormSenderNode::OnRepairTimeout()

void NormSenderNode::UpdateRecvRate(const struct timeval& currentTime, unsigned short msgSize)
{
    if (prev_update_time.tv_sec || prev_update_time.tv_usec)
    {
        double interval = (double)(currentTime.tv_sec - prev_update_time.tv_sec);
        if (currentTime.tv_sec > prev_update_time.tv_sec)
            interval += 1.0e-06*(double)(currentTime.tv_usec - prev_update_time.tv_usec);
        else
            interval -= 1.0e-06*(double)(prev_update_time.tv_usec - currentTime.tv_usec);            
        double measurementInterval = rtt_confirmed ? rtt_estimate : grtt_estimate;
        // Here, we put a NORM_TICK_MIN sec lower bound on our measurementInterval for the 
        // recv_rate because of the typical limited granularity of our system clock
        // (Note this can limit our ramp up of data rate during slow start)
        if (measurementInterval < NORM_TICK_MIN) measurementInterval = NORM_TICK_MIN;
        recv_accumulator.Increment(msgSize); 
        if (interval > 0.0)
        {
            double currentRecvRate = recv_accumulator.GetScaledValue(1.0 / interval);
            if ((interval >= measurementInterval) && (currentRecvRate < recv_rate))
            {
                // Make sure we've allowed sufficient time for a measurement at low rates
                double nominalSize = (nominal_packet_size > segment_size) ? nominal_packet_size : segment_size;
                double minInterval = 4.0 * nominalSize / recv_rate;
                if (measurementInterval < minInterval) measurementInterval = minInterval;
            }
            if (interval >= measurementInterval)
            {
                recv_rate = recv_rate_prev = currentRecvRate; 
                prev_update_time = currentTime;
                recv_accumulator.Reset();
            }
            else if (0.0 == recv_rate)
            {
                recv_rate = currentRecvRate;
                recv_rate_prev = 0.0;
            }
            else if (slow_start)
            {
                // Go ahead and allow estimate to slew upwards on new packet arrivals
                // (helps "slow start" ramp up a little more cleanly)
                double rateDelta = currentRecvRate - recv_rate_prev;
                if (rateDelta > 0.0)
                {
                    double scale = interval / measurementInterval;
                    double partialRate = recv_rate_prev + scale*rateDelta;
                    if (partialRate > recv_rate) recv_rate = partialRate;
                }
            }
        }
        else if (0.0 == recv_rate)
        {
            // Approximate initial recv_rate when initial packets arrive in a burst
            recv_rate = recv_accumulator.GetValue() / NORM_TICK_MIN;
            recv_rate_prev = 0.0;
        }
        nominal_packet_size += 0.05 * (((double)msgSize) - nominal_packet_size); 
    }
    else
    {
        recv_rate = recv_rate_prev = 0.0;  
        prev_update_time = currentTime;
        recv_accumulator.Reset();
        nominal_packet_size = msgSize;
    }
    
}  // end NormSenderNode::UpdateRecvRate()

void NormSenderNode::Activate(bool isObjectMsg)
{
    if (!activity_timer.IsActive())
    {
        double activityInterval = 2*session.GetTxRobustFactor()*grtt_estimate;
        if (activityInterval < ACTIVITY_INTERVAL_MIN) activityInterval = ACTIVITY_INTERVAL_MIN;
        activity_timer.SetInterval(activityInterval);
        activity_timer.SetRepeat(robust_factor);
        session.ActivateTimer(activity_timer);
        sender_active = false;
        // If it is _not_ an object msg, do a comprehensive repair check
        // to re-initiate NACKing for any missing data from prior sender
        // activity (iff rx_pending_mask.IsSet())
        // (If it is an object message, RepairCheck() will be called accordingly
        if (!isObjectMsg && rx_pending_mask.IsSet())
            RepairCheck(NormObject::THRU_OBJECT, max_pending_object, 0, 0); // (TBD) thru object???
        session.Notify(NormController::REMOTE_SENDER_ACTIVE, this, NULL);
    }
    else if (isObjectMsg)
    {
        sender_active = true;
    }
}  // end NormSenderNode::Activate()

bool NormSenderNode::OnActivityTimeout(ProtoTimer& /*theTimer*/)
{
    if (sender_active)
    {
        activity_timer.ResetRepeat();
    }
    else if (0 == activity_timer.GetRepeatCount())
    {
        // Serve completely inactive?
        PLOG(PL_ERROR, "NormSenderNode::OnActivityTimeout() node>%lu sender>%lu gone inactive?\n",
                LocalNodeId(), GetId());
        //FreeBuffers();  This now needs to be done by the app as of norm version 1.4b3
        session.Notify(NormController::REMOTE_SENDER_INACTIVE, this, NULL);
    }
    else
    {
        PLOG(PL_INFO, "NormSenderNode::OnActivityTimeout() node>%lu for sender>%lu\n",
            LocalNodeId(), GetId());
        struct timeval currentTime;
        ::ProtoSystemTime(currentTime);
        UpdateRecvRate(currentTime, 0);
        if (synchronized)
        {
            NormObject* objMax = rx_table.Find(max_pending_object);
            if (NULL != objMax)
            {
                /*NormSegmentId segMax = objMax->GetMaxPendingSegmentId();
                if (0 != segMax)
                    RepairCheck(NormObject::THRU_SEGMENT, 
                                max_pending_object,
                                objMax->GetMaxPendingBlockId(), 
                                objMax->GetMaxPendingSegmentId() - 1);
                else
                    RepairCheck(NormObject::TO_BLOCK, 
                                max_pending_object,
                                objMax->GetMaxPendingBlockId(), 
                                0);*/
                // Let's try this instead
                RepairCheck(NormObject::THRU_OBJECT,   // (TBD) thru object???
                            max_pending_object, 0, 0);
                
                // (TBD) What should we really do here?  Our current NormNode::RepairCheck() and
                //       NormObject::ReceiverRepairCheck() methods update the "max_pending" indices
                //       so we _could_ make ourselves NACK for more repair than we should
                //       when the inactivity timeout kicks in ???  But if we don't NACK "thru object"
                //       we may miss something at end-of-transmission by not not NACKing?  I guess
                //       the reliability really is in the flush process and our activity timeout NACK
                //       is "iffy, at best" ... Perhaps we need to have some sort of "wildcard" NACK,
                //       but then _everyone_ would NACK at EOT all the time, often for nothing ... so
                //       I guess the activity timeout NACK isn't perfect ... but could help some
                //       so we leave it as it is for the moment ("THRU_OBJECT") ... perhaps we could
                //       add a parameter so NormObject::ReceiverRepairCheck() doesn't update its
                //       "max_pending" indices - or would this break NACK building
            }
            else
            {
                RepairCheck(NormObject::THRU_OBJECT,   
                            max_pending_object, 0, 0);
                //RepairCheck(NormObject::TO_BLOCK,   // (TBD) thru object???
                //            max_pending_object, 0, 0);
            }
        }
        // We manually managed the "repeat_count" here to avoid the
        // case where "bursty" receiver scheduling may lead to false
        // inactivity indication
        int repeatCount = activity_timer.GetRepeatCount();
        if (repeatCount > 0) repeatCount--;
        activity_timer.Deactivate();
        session.ActivateTimer(activity_timer);
        activity_timer.SetRepeatCount(repeatCount);
        sender_active = false;
        return false; // since we manually deactivated/reactivated the timer
    }
    sender_active = false;
    return true;     
}  // end NormSenderNode::OnActivityTimeout()
  
bool NormSenderNode::UpdateLossEstimate(const struct timeval& currentTime, 
                                        unsigned short        seq, 
                                        bool                  ecnStatus)
{
    if (loss_estimator.Update(currentTime, seq, ecnStatus))
    {
        if (slow_start)
        {
            // Calculate loss initialization based on current receive rate
            // and rtt estimation 
            double nominalSize = (nominal_packet_size > segment_size) ? nominal_packet_size : segment_size;
            double lossInit = nominalSize / (recv_rate * rtt_estimate);
            lossInit *= lossInit;
            lossInit *= 3.0/2.0;
            double altLoss = (double)loss_estimator.LastLossInterval();
            if (altLoss < 2.0) altLoss = 2.0;  // makes sure it's no worse than 50% pkt loss
            double altInit = 1.0 / altLoss;
            if (altInit < lossInit) lossInit = altInit;
            loss_estimator.SetInitialLoss(lossInit);
            slow_start = false;
        }   
        // TBD - schedule immediate CC feedback if CLR? - note duplicate feedback issue so
        //       need to do this with preemptively incremented cc_sequence value ...
        // TBD - This can cause extra ACK for non-cc unicast sessions.
        // We could reset "cc_feedback_needed" to false if NACK feedback is scheduled
        // for non-clr unicast case instead???
        if (cc_enable && (is_clr || is_plr))// || !session.Address().IsMulticast()))
            cc_feedback_needed = true;
        return true;
    }
    else
    {
        return false;
    }
}  // end NormSenderNode::UpdateLossEstimate()

void NormSenderNode::CheckCCFeedback()
{
    // "cc_feedback_needed" is set to "true" if a loss event occurs
    // and remains "true" no cc feedback was sent otherwise
    // (gets reset to "false" when OnCCTimeout() is called here)
    if (cc_feedback_needed)
    {
        cc_sequence++;  // so sender won't ignore as duplicate feedback
        if (cc_timer.IsActive()) cc_timer.Deactivate();
        cc_timer.ResetRepeat(); // makes sure timer phase is correct
        OnCCTimeout(cc_timer);
    }
}  // end NormSenderNode::CheckCCFeedback()

void NormSenderNode::AttachCCFeedback(NormAckMsg& ack)
{
    // GRTT response is deferred until transmit time
    NormCCFeedbackExtension ext;
    ack.AttachExtension(ext);
    if (is_clr) 
        ext.SetCCFlag(NormCC::CLR);
    else if (is_plr)
        ext.SetCCFlag(NormCC::PLR);
    if (rtt_confirmed)
        ext.SetCCFlag(NormCC::RTT);
    ext.SetCCRtt(rtt_quantized);
    double ccLoss = slow_start ? 0.0 : LossEstimate(); 
    
    //UINT16 lossQuantized = NormQuantizeLoss(ccLoss);
    //ext.SetCCLoss(lossQuantized);
    UINT32 lossQuantized = NormQuantizeLoss32(ccLoss);
    ext.SetCCLoss32(lossQuantized);
    //if (0.0 == ccLoss)
    if (0 == lossQuantized)
    {
        ext.SetCCFlag(NormCC::START);
        ext.SetCCRate(NormQuantizeRate(2.0 * recv_rate));
    }
    else
    {
        //double nominalSize = (nominal_packet_size > segment_size) ? nominal_packet_size : segment_size;
        double nominalSize = (0 != nominal_packet_size) ? nominal_packet_size : segment_size;
        double ccRate = NormSession::CalculateRate(nominalSize, rtt_estimate, ccLoss);
#ifdef LIMIT_CC_RATE                     
        // Experimental modification to NORM-CC where congestion control rate is limited
        // to MIN(2.0*measured recv rate, calculated rate).  This might prevent large rate
        // overshoot in conditions where the loss measurement (perhaps initial loss) is 
        // very low due to big network packet buffers, etc   
        double rxRate = 2.0*recv_rate;
        if (rxRate < ccRate)
        {
            ext.SetCCFlag(NormCC::LIMIT);
            ccRate = rxRate;
        }
#endif // LIMIT_CC_RATE
        ext.SetCCRate(NormQuantizeRate(ccRate));
    }
    PLOG(PL_DEBUG, "NormSenderNode::AttachCCFeedback() node>%lu sending ACK rate:%lf kbps (rtt:%lf loss:%lf s:%lf recvRate:%lf) slow_start:%d\n",
                   LocalNodeId(), 8.0e-03*NormUnquantizeRate(ext.GetCCRate()) , 
                   rtt_estimate, ccLoss, nominal_packet_size, 8.0e-03*recv_rate, slow_start);
    ext.SetCCSequence(cc_sequence);
}  // end NormSenderNode::AttachCCFeedback()

bool NormSenderNode::OnCCTimeout(ProtoTimer& /*theTimer*/)
{
    // Build and send NORM_ACK(CC)
    switch (cc_timer.GetRepeatCount())
    {
        case 0:
            // "hold-off" time has ended
            break;
            
        case 1:
        {
            // We weren't suppressed, so build an ACK(RTT) and send
            NormAckMsg* ack = (NormAckMsg*)session.GetMessageFromPool();
            if (!ack)
            {
                PLOG(PL_WARN, "NormSenderNode::OnCCTimeout() node>%lu warning: "
                        "message pool empty ...\n", LocalNodeId());
                if (cc_timer.IsActive()) cc_timer.Deactivate();
                return false;   
            }
            ack->Init();
            ack->SetSenderId(GetId());
            ack->SetInstanceId(instance_id);
            ack->SetAckType(NormAck::CC);
            ack->SetAckId(0);
            
            AttachCCFeedback(*ack);  // cc feedback extension
            
            if (unicast_nacks)
                ack->SetDestination(GetAddress());
            else
                ack->SetDestination(session.Address());
            session.SendMessage(*ack);
            session.ReturnMessageToPool(ack);
            cc_feedback_needed = false;
            
            // Begin cc_timer "holdoff" phase
            if (!is_clr && !is_plr && session.Address().IsMulticast())
            {
                cc_timer.SetInterval(grtt_estimate*backoff_factor);
                return true;
            }
            else if (cc_timer.IsActive()) 
            {
                cc_timer.Deactivate();
            }
            break;
        }
        
        default:
            // Should never occur
            ASSERT(0);
            break;
    }
    return true;
}  // end NormSenderNode::OnCCTimeout()

bool NormSenderNode::OnAckTimeout(ProtoTimer& /*theTimer*/)
{
    // Build and send NORM_ACK(FLUSH)
    NormAckFlushMsg* ack = (NormAckFlushMsg*)session.GetMessageFromPool();
    if (ack)
    {
        ack->Init();
        ack->SetSenderId(GetId());
        ack->SetInstanceId(instance_id);
        ack->SetAckType(NormAck::FLUSH);
        ack->SetAckId(0);
        AttachCCFeedback(*ack);
        ack->SetObjectId(watermark_object_id);
        
        // _Attempt_ to set the fec_payload_id source block length field appropriately
        UINT16 blockLen;
        NormObject* obj = rx_table.Find(watermark_object_id);
        if (NULL != obj)
            blockLen = obj->GetBlockSize(watermark_block_id);
        else if (watermark_segment_id < ndata)
            blockLen = ndata;
        else
            blockLen = watermark_segment_id;
        ack->SetFecPayloadId(fec_id, watermark_block_id, watermark_segment_id, blockLen, fec_m);
        
        if (unicast_nacks)
            ack->SetDestination(GetAddress());
        else
            ack->SetDestination(session.Address());
        
        // Don't rate limit feedback messages
        session.SendMessage(*ack);
        session.ReturnMessageToPool(ack);
        cc_feedback_needed = false;
        if (cc_enable && !is_clr && !is_plr && session.Address().IsMulticast())
        {
            // Install cc feedback holdoff
            cc_timer.SetInterval(grtt_estimate*backoff_factor);
            if (cc_timer.IsActive())
                cc_timer.Reschedule();
            else
                session.ActivateTimer(cc_timer);
            cc_timer.DecrementRepeatCount();  // put timer into "holdoff" phase
        } 
        else if (cc_timer.IsActive())
        {
            cc_timer.Deactivate();
            return false;
        }
    }
    else
    {
        PLOG(PL_WARN, "NormSenderNode::OnAckTimeout() warning: message pool exhausted!\n");
    }
    return true;
}  // end NormSenderNode::OnAckTimeout()


NormAckingNode::NormAckingNode(class NormSession& theSession, NormNodeId nodeId)
 : NormNode(theSession, nodeId), ack_received(false), req_count(theSession.GetTxRobustFactor())
{
    
}

NormAckingNode::~NormAckingNode()
{
}

NormNodeTree::NormNodeTree()
 : root(NULL)
{

}

NormNodeTree::~NormNodeTree()
{
    Destroy();
}


NormNode *NormNodeTree::FindNodeById(NormNodeId nodeId) const
{
    NormNode* x = root;
    while(x && (x->id != nodeId))
    {
		if (nodeId < x->id)
            x = x->left;
        else
            x = x->right;
    }
    return x;   
}  // end NormNodeTree::FindNodeById() 



void NormNodeTree::AttachNode(NormNode *node)
{
    ASSERT(NULL != node);
    node->Retain();
    node->left = NULL;
    node->right = NULL;
    NormNode *x = root;
    while (x)
    {
        if (node->id < x->id)
        {
            if (!x->left)
            {
                x->left = node;
                node->parent = x;
                return;
            }
            else
            {
                x = x->left;
            }
        }
        else
        {
           if (!x->right)
           {
               x->right = node;
               node->parent = x;
               return;
           }
           else
           {
               x = x->right;
           }   
        }
    }
    root = node;  // root _was_ NULL
}  // end NormNodeTree::AttachNode()


void NormNodeTree::DetachNode(NormNode* node)
{
    ASSERT(NULL != node);
    node->Release();
    NormNode* x;
    NormNode* y;
    if (!node->left || !node->right)
    {
        y = node;
    }
    else
    {
        if (node->right)
        {
            y = node->right;
            while (y->left) y = y->left;
        }
        else
        {
            x = node;
            y = node->parent;
            while(y && (y->right == x))
            {
                x = y;
                y = y->parent;
            }
        }
    }
    if (y->left)
        x = y->left;
    else
        x = y->right;
    if (x) x->parent = y->parent;
    if (!y->parent)
        root = x;
    else if (y == y->parent->left)
        y->parent->left = x;
    else
        y->parent->right = x;
    
    if (node != y)
    {
        if ((y->parent = node->parent))
        {
            if (y->id < y->parent->id)
                y->parent->left = y;
            else
                y->parent->right = y;
        }
        else
        {
            root = y;
        }
        if ((y->left = node->left)) y->left->parent = y;
        if ((y->right = node->right)) y->right->parent = y;
    }        
}  // end NormNodeTree::DetachNode()


void NormNodeTree::Destroy()
{
    NormNode* n;
    while ((n = root)) 
    {
        DetachNode(n);
        n->Release();
    }
}  // end NormNodeTree::Destroy()

NormNodeTreeIterator::NormNodeTreeIterator(const NormNodeTree& t, NormNode* prevNode)
 : tree(t)
{
    Reset(prevNode);
}  

void NormNodeTreeIterator::Reset(NormNode* prevNode)
{
    NormNode* x = tree.root;
    if (NULL != x)
    {
        if (NULL == prevNode)
        {
            while (x->left) x = x->left;
            next = x;
        }
        else
        {
            next = prevNode;
            GetNextNode();  // sets "next" to return subsequent node
        }
    }
    else
    {
        next = NULL;   
    }
}  // end NormNodeTreeIterator::Reset()

NormNode* NormNodeTreeIterator::GetNextNode()
{
    NormNode* n = next;
    if (n)
    {
        if (next->right)
        {
            NormNode* y = n->right;
            while (y->left) y = y->left;
            next = y;
        }
        else
        {
            NormNode* x = n;
            NormNode* y = n->parent;
            while(y && (y->right == x))
            {
                x = y;
                y = y->parent;
            }
            next = y;
        }
    }
    return n;
}  // end NormNodeTreeIterator::GetNextNode()

NormNodeList::NormNodeList()
  : head(NULL), tail(NULL), count(0)
{
}

NormNodeList::~NormNodeList()
{
    Destroy();
}

NormNode* NormNodeList::FindNodeById(NormNodeId nodeId) const
{
    NormNode *next = head;
    while (next)
    {
        if (nodeId == next->id)
            return next;
        else
            next = next->right;
    }
    return NULL;
}  // NormNodeList::Find()

void NormNodeList::Append(NormNode *theNode)
{
    ASSERT(NULL != theNode);
    theNode->Retain();
    theNode->left = tail;
    if (tail)
        tail->right = theNode;
    else
        head = theNode;
    tail = theNode;
    theNode->right = NULL;
    count++;
}  // end NormNodeList::Append()

void NormNodeList::Remove(NormNode *theNode)
{
    ASSERT(NULL != theNode);
    theNode->Release();
    if (theNode->right)
        theNode->right->left = theNode->left;
    else
        tail = theNode->left;
    if (theNode->left)
        theNode->left->right = theNode->right;
    else
        head = theNode->right;
    count--;
}  // end NormNodeList::Remove()

void NormNodeList::Destroy()
{
    NormNode* n;
    while ((n = head))
    {
        Remove(n);
        n->Release();
    }   
}  // end NormNodeList::Destroy()


//////////////////////////////////////////////////////////
//
// NormLossEstimator implementation
//

NormLossEstimator::NormLossEstimator()
 : synchronized(false), seeking_loss_event(true), event_window(0.0)
{
    event_time.tv_sec = event_time.tv_usec = 0;
    memset(history, 0, (DEPTH+1)*sizeof(unsigned int));
}

const double NormLossEstimator::weight[DEPTH] = 
{
    1.0, 1.0, 1.0, 1.0,
    0.8, 0.6, 0.4, 0.2  
};

int NormLossEstimator::SequenceDelta(unsigned short a, unsigned short b)
{
    int delta = a - b;
    if (delta < -0x8000)
        return (delta + 0x10000);
    else if (delta < 0x8000)
        return delta;
    else 
        return delta - 0x10000; 
}  // end NormLossEstimator::SequenceDelta()

// Returns true when a loss event has occurred
bool NormLossEstimator::Update(const struct timeval&    currentTime,
                               unsigned short           seq, 
                               bool                     ecn)
{
    if (!synchronized)
    {   
        Sync(seq);
        return false;
    }
    bool outage = false;
    int delta = SequenceDelta(seq, index_seq);
    if (abs(delta) > MAX_OUTAGE)    // out-of-range packet
    {
        index_seq = seq;
        return false;
    }    
    else if (delta > 0)             // new packet arrival
    {
        if (ecn || (delta > 1)) outage = true;
        index_seq = seq;
    }
    else // (delta <= 0)            // old misordered or duplicate packet
    {
        return false;               
    }        
    if (outage)
    {
        if (!seeking_loss_event)
        {
            double deltaTime = (double)(currentTime.tv_sec - event_time.tv_sec);
            if (currentTime.tv_usec > event_time.tv_usec)
                deltaTime += (double)(currentTime.tv_usec - event_time.tv_usec) * 1.0e-06;
            else
                deltaTime -= (double)(event_time.tv_usec - currentTime.tv_usec) * 1.0e-06;
            if (deltaTime > event_window) seeking_loss_event = true;
        }
        if (seeking_loss_event)
        {
            memmove(history+1, history, DEPTH*sizeof(unsigned int));
            history[0] = 1;
            seeking_loss_event = false; 
            event_time = currentTime;  
            return true;
        }
        else
        {
            // Only count one loss per loss event
            history[0] = 1;
            return false;
        } 
    }
    else
    {
        history[0]++;
        return false;           
    }    
}  // end NormLossEstimator::Update()

double NormLossEstimator::LossFraction()
{
    if (0 == history[1]) return 0.0;
    double weightSum = 0.0;
    double s0 = 0.0;
    const double* wptr = weight;
    const unsigned int* h = history;
    unsigned int i;
    for (i = 0; i < DEPTH; i++)
    {
        if (0 == *h) break;
        s0 += *wptr * *h++;
        weightSum += *wptr++;
    }
    s0 /= weightSum;
    
    weightSum = 0.0;
    double s1 = 0.0;
    wptr = weight;
    h = history + 1;
    for (i = 0; i < DEPTH; i++)
    {
        if (0 == *h) break;
        s1 += *wptr * *h++;  // ave loss interval w/out current interval
        weightSum += *wptr++; // (TBD) this could be pre-computed
    }
    s1 /= weightSum;
    return (1.0 / (MAX(s0,s1)));
}  // end NormLossEstimator::LossFraction()



NormLossEstimator2::NormLossEstimator2()
    : init(false), ignore_loss(false), tolerate_loss(false),
      lag_mask(0xffffffff), lag_depth(0), lag_test_bit(0x01),
      event_window(0.0), seeking_loss_event(SEEKING), current_discount(1.0)
{
    event_time.tv_sec = event_time.tv_usec = 0;
    memset(history, 0, 9*sizeof(unsigned long));
    discount[0] = 1.0;
}

const double NormLossEstimator2::weight[8] = 
{
    1.0, 1.0, 1.0, 1.0,
    0.8, 0.6, 0.4, 0.2  
};
        
int NormLossEstimator2::SequenceDelta(unsigned short a, unsigned short b)
{
    int delta = a - b;
    if (delta < -0x8000)
        return (delta + 0x10000);
    else if (delta < 0x8000)
        return delta;
    else 
        return delta - 0x10000; 
}  // end NormLossEstimator2::SequenceDelta()


bool NormLossEstimator2::Update(const struct timeval&   currentTime,
                                unsigned short          theSequence, 
                                bool                    ecnStatus)
{
    // (TBD) What if the first packet that arrives has ECN set???
    if (!init) 
    {
        Init(theSequence); 
        return false;
    } 
    unsigned int outageDepth = 0;
    // Process packet through lag filter and check for loss
    int delta = SequenceDelta(theSequence, lag_index);  
    if (delta > 100)        // Very new packet arrived
    {
        Sync(theSequence);  // resync
        return false;   
    }
    else if (delta > 0)     // New packet arrived
    {
        if (lag_depth)
        {
            unsigned int outage = 0;
            for (int i = 0; i < delta; i++)
            {
                if (i <= (int)lag_depth)
                {
                    outage++;
                    if (lag_mask & lag_test_bit)
                    {
                        if (outage > 1) 
                            outageDepth = MAX(outage, outageDepth);
                        outage = 0;  
                    }
                    else
                    {
                        lag_mask |= lag_test_bit;
                    }
                    lag_mask <<= 1;
                } 
                else
                {
                    outage += delta - lag_depth - 1; 
                    break;
                }
            }
            outageDepth = MAX(outage, outageDepth);
            lag_mask |= 0x01;
        }
        else
        {
            if (delta > 1) outageDepth = delta - 1;
        }
        lag_index = theSequence;
    }
    else if (delta < -100)              // Very old packet arrived
    {
         Sync(theSequence); // resync
         return false; 
    }
    else if (delta < -((int)lag_depth)) // Old packet arrived
    {
        ChangeLagDepth(-delta);
    }
    else if (delta < 0)                 // Lagging packet arrived
    {                                   // (duplicates have no effect)
        lag_mask |= (0x01 << (-delta));
        return false;
    }
    else // (delta == 0)
    {
        return false;                    // Duplicate packet arrived, ignore
    }        
    
    if (ignore_loss) outageDepth = 0;
    
    if (ecnStatus) outageDepth += 1;
    
    bool newLossEvent = false;
    
    //if (!seeking_loss_event)
    if (SEEKING != seeking_loss_event)
    {
        double deltaTime = (double)(currentTime.tv_sec - event_time.tv_sec);
        if (currentTime.tv_usec > event_time.tv_usec)
            deltaTime += (double)(currentTime.tv_usec - event_time.tv_usec) * 1.0e-06;
        else
            deltaTime -= (double)(event_time.tv_usec - currentTime.tv_usec) * 1.0e-06;
        // Use a longer "loss event window" for NORM-CCE ("ignore_loss" = "true")
        // since RED/ECN tends to start marking "early" and stop marking "late"
        double windowScale = ignore_loss ? 2.0 : 1.0;
        if (deltaTime > windowScale*event_window)
        { 
            //if (CONFIRMING == seeking_loss_event)
            //    TRACE("Clearing unconfirmed loss event ...\n");
            seeking_loss_event = SEEKING;
        }
    }
    
    //if (seeking_loss_event)
    if (CONFIRMED != seeking_loss_event)
    {
        if ((1 == outageDepth) && !ecnStatus && tolerate_loss)  // single, non-ECN loss event
        {
            if (SEEKING == seeking_loss_event)
            {
                seeking_loss_event = CONFIRMING;  // wait for more loss to confirm congestion event
                event_time = event_time_orig = currentTime;
                outageDepth = 0;
                //TRACE("Deferring loss event for confirmation ...\n");
            }
        }
        
        if (outageDepth)  // non-zero outageDepth means pkt loss(es)
        {
            // call to LossFraction() here is just to make sure "current_discount"
            // is updated accordlingly
            LossFraction();
            // New method
            // New loss event, shift loss interval history & discounts
            memmove(&history[1], &history[0], 8*sizeof(unsigned long));
            history[0] = 0;
            for (int i = 8; i > 0; i--)
                discount[i] = discount[i-1]*current_discount;
            discount[0] = 1.0;
            current_discount = 1.0;
            //TRACE("Confirmed loss event ...(tolerate:%d)\n", tolerate_loss);
            seeking_loss_event = CONFIRMED;
            event_time = event_time_orig = currentTime;
            newLossEvent = true;
            
            //TRACE("time>%lf new loss event fraction = %lf\n", ProtoTime(currentTime).GetValue(), LossFraction());
            
        }
    }  
    else
    {
        // we commented this out not to reset history to get better loss event period measurement
        // if (outageDepth > 0)
        //  history[0] = 0;
    }  // end if/else (seeking_loss_event)
    
    // TBD - instead of counting packets, should we calculate based on the sequence number 
    // of the last loss event and the current sequence number? (so dups won't fool us into increasing rate)
    //if (history[0] < 65536*2) 
        history[0]++;
    
    return newLossEvent;
}  // end NormLossEstimator2::Update()


// TFRC Loss interval averaging with discounted, weighted averaging
double NormLossEstimator2::LossFraction()
{
    if (0 == history[1]) return 0.0;   
    // Compute older weighted average s1->s8 for discount determination  
    double average = 0.0;
    double scaling = 0.0;
	unsigned int i;
    for (i = 1; i < 9; i++)
    {
        if (history[i])
        {
            average += history[i] * weight[i-1] * discount[i];
            scaling += discount[i] * weight[i-1];
        }
        else
        {
            break;
        }
    }
    double s1 = average / scaling;

    // Compute discount if applicable 
    if (history[0] > (2.0*s1))
    {
        current_discount = (2.0*s1) / (double) history[0];
        current_discount = MAX(current_discount, 0.5);
    }
    if (history[0] > s1) return (1.0 / (double)history[0]);
    
    // Compute newer weighted average s0->s7 with discounting
    average = 0.0;
    scaling = 0.0;
	for (i = 0; i < 8; i++)
    {
        if (history[i])
        {
            double d = (i > 0) ? current_discount : 1.0;
            average += d * history[i] * weight[i] * discount[i];
            scaling += d * discount[i] * weight[i];
        }
        else
        {
            break;
        }
    }
    double s0 = (average > 0.0) ? average / scaling : 0.0;
    
    // Use max of old/new averages (i.e. only use discounting if it helps increase rate)
    double result = (1.0 /  MAX(s0, s1));
    
    return result;
}  // end NormLossEstimator2::LossFraction()
