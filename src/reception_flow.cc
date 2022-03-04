#include "reception_flow.hh"

#include "random.hh"

#include "uvgrtp/util.hh"
#include "uvgrtp/frame.hh"
#include "uvgrtp/socket.hh"
#include "uvgrtp/debug.hh"

#include <chrono>

#ifndef _WIN32
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#else
#define MSG_DONTWAIT 0
#endif

#include <cstring>

constexpr size_t RECV_BUFFER_SIZE = 0xffff - IPV4_HDR_SIZE - UDP_HDR_SIZE;

constexpr size_t DEFAULT_INITIAL_BUFFER_SIZE = 4194304;


uvgrtp::reception_flow::reception_flow() :
    recv_hook_arg_(nullptr),
    recv_hook_(nullptr),
    should_stop_(true),
    receiver_(nullptr),
    ring_buffer_(),
    ring_read_index_(-1), // invalid first index that will increase to a valid one
    last_ring_write_index_(0),
    buffer_size_kbytes_(DEFAULT_INITIAL_BUFFER_SIZE)
{
    create_ring_buffer();
}

uvgrtp::reception_flow::~reception_flow()
{
    destroy_ring_buffer();
    clear_frames();
}

void uvgrtp::reception_flow::clear_frames()
{
    frames_mtx_.lock();
    for (auto& frame : frames_)
    {
        delete[] frame;
    }

    frames_.clear();
    frames_mtx_.unlock();
}

void uvgrtp::reception_flow::create_ring_buffer()
{
    destroy_ring_buffer();
    size_t elements = buffer_size_kbytes_ / RECV_BUFFER_SIZE;

    for (int i = 0; i < elements; ++i)
    {
        ring_buffer_.push_back({ new uint8_t[RECV_BUFFER_SIZE] , 0 });
    }
}

void uvgrtp::reception_flow::destroy_ring_buffer()
{
    for (int i = 0; i < ring_buffer_.size(); ++i)
    {
        delete[] ring_buffer_.at(i).data;
    }
    ring_buffer_.clear();
}

void uvgrtp::reception_flow::set_buffer_size(ssize_t& value)
{
    buffer_size_kbytes_ = value;
    create_ring_buffer();
}

rtp_error_t uvgrtp::reception_flow::start(std::shared_ptr<uvgrtp::socket> socket, int flags)
{
    should_stop_ = false;
    processor_ = std::unique_ptr<std::thread>(new std::thread(&uvgrtp::reception_flow::process_packet, this, flags));
    receiver_ = std::unique_ptr<std::thread>(new std::thread(&uvgrtp::reception_flow::receiver, this, socket, flags));

    // set receiver thread priority to maximum priority
    LOG_DEBUG("Trying to set receiver thread realtime priority");

#ifndef WIN32
    struct sched_param params;
    params.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(receiver_->native_handle(), SCHED_FIFO, &params);
    params.sched_priority = sched_get_priority_max(SCHED_FIFO) - 1;
    pthread_setschedparam(processor_->native_handle(), SCHED_FIFO, &params);
#else

    SetThreadPriority(receiver_->native_handle(), REALTIME_PRIORITY_CLASS);
    SetThreadPriority(processor_->native_handle(), ABOVE_NORMAL_PRIORITY_CLASS);

#endif

    return RTP_ERROR::RTP_OK;
}

rtp_error_t uvgrtp::reception_flow::stop()
{
    should_stop_ = true;
    process_cond_.notify_all();

    if (receiver_ != nullptr && receiver_->joinable())
    {
        receiver_->join();
    }

    if (processor_ != nullptr && processor_->joinable())
    {
        processor_->join();
    }

    clear_frames();

    return RTP_OK;
}

rtp_error_t uvgrtp::reception_flow::install_receive_hook(
    void *arg,
    void (*hook)(void *, uvgrtp::frame::rtp_frame *)
)
{
    if (!hook)
        return RTP_INVALID_VALUE;

    recv_hook_     = hook;
    recv_hook_arg_ = arg;

    return RTP_OK;
}

uvgrtp::frame::rtp_frame *uvgrtp::reception_flow::pull_frame()
{
    while (frames_.empty() && !should_stop_)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (should_stop_)
        return nullptr;

    frames_mtx_.lock();
    auto frame = frames_.front();
    frames_.erase(frames_.begin());
    frames_mtx_.unlock();

    return frame;
}

uvgrtp::frame::rtp_frame *uvgrtp::reception_flow::pull_frame(size_t timeout_ms)
{
    auto start_time = std::chrono::high_resolution_clock::now();

    while (frames_.empty() && 
        !should_stop_ &&
        timeout_ms > std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time).count())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (should_stop_ || frames_.empty())
        return nullptr;

    frames_mtx_.lock();
    auto frame = frames_.front();
    frames_.erase(frames_.begin());
    frames_mtx_.unlock();

    return frame;
}

uint32_t uvgrtp::reception_flow::install_handler(uvgrtp::packet_handler handler)
{
    uint32_t key;

    if (!handler)
        return 0;

    do {
        key = uvgrtp::random::generate_32();
    } while (!key || (packet_handlers_.find(key) != packet_handlers_.end()));

    packet_handlers_[key].primary = handler;
    return key;
}

rtp_error_t uvgrtp::reception_flow::install_aux_handler(
    uint32_t key,
    void *arg,
    uvgrtp::packet_handler_aux handler,
    uvgrtp::frame_getter getter
)
{
    if (!handler)
        return RTP_INVALID_VALUE;

    if (packet_handlers_.find(key) == packet_handlers_.end())
        return RTP_INVALID_VALUE;

    auxiliary_handler aux;
    aux.arg = arg;
    aux.getter = getter;
    aux.handler = handler;

    packet_handlers_[key].auxiliary.push_back(aux);
    return RTP_OK;
}

rtp_error_t uvgrtp::reception_flow::install_aux_handler_cpp(uint32_t key,
    std::function<rtp_error_t(int, uvgrtp::frame::rtp_frame**)> handler,
    std::function<rtp_error_t(uvgrtp::frame::rtp_frame**)> getter)
{
    if (!handler)
        return RTP_INVALID_VALUE;

    if (packet_handlers_.find(key) == packet_handlers_.end())
        return RTP_INVALID_VALUE;

    auxiliary_handler_cpp ahc = {handler, getter};
    packet_handlers_[key].auxiliary_cpp.push_back(ahc);
    return RTP_OK;
}

void uvgrtp::reception_flow::return_frame(uvgrtp::frame::rtp_frame *frame)
{
    if (recv_hook_) {
        recv_hook_(recv_hook_arg_, frame);
    } else {
        frames_mtx_.lock();
        frames_.push_back(frame);
        frames_mtx_.unlock();
    }
}

void uvgrtp::reception_flow::call_aux_handlers(uint32_t key, int flags, uvgrtp::frame::rtp_frame **frame)
{
    rtp_error_t ret;

    for (auto& aux : packet_handlers_[key].auxiliary) {
        switch ((ret = (*aux.handler)(aux.arg, flags, frame))) {
            /* packet was handled successfully */
            case RTP_OK:
                break;

            case RTP_MULTIPLE_PKTS_READY:
            {
                while ((*aux.getter)(aux.arg, frame) == RTP_PKT_READY)
                    this->return_frame(*frame);
            }
            break;

            case RTP_PKT_READY:
                this->return_frame(*frame);
                break;

            /* packet was not handled or only partially handled by the handler
             * proceed to the next handler */
            case RTP_PKT_NOT_HANDLED:
            case RTP_PKT_MODIFIED:
                continue;

            case RTP_GENERIC_ERROR:
                LOG_DEBUG("Received a corrupted packet!");
                break;

            default:
                LOG_ERROR("Unknown error code from packet handler: %d", ret);
                break;
        }
    }

    for (auto& aux : packet_handlers_[key].auxiliary_cpp) {
        switch ((ret = aux.handler(flags, frame))) {
            
        case RTP_OK: /* packet was handled successfully */
        {
            break;
        }
        case RTP_MULTIPLE_PKTS_READY:
        {
            while (aux.getter(frame) == RTP_PKT_READY)
                this->return_frame(*frame);

            break;
        }
        case RTP_PKT_READY:
        {
            this->return_frame(*frame);
            break;
        }

            /* packet was not handled or only partially handled by the handler
             * proceed to the next handler */
        case RTP_PKT_NOT_HANDLED:
        {
            continue;
        }
        case RTP_PKT_MODIFIED:
        {
            continue;
        }

        case RTP_GENERIC_ERROR:
        {
            LOG_DEBUG("Received a corrupted packet!");
            break;
        }

        default:
        {
            LOG_ERROR("Unknown error code from packet handler: %d", ret);
            break;
        }
        }
    }
}

void uvgrtp::reception_flow::receiver(std::shared_ptr<uvgrtp::socket> socket, int flags)
{
    LOG_DEBUG("Start reception loop");

    while (!should_stop_) {

        // First we wait using poll until there is data in the socket

#ifdef _WIN32
        LPWSAPOLLFD pfds = new pollfd();
#else
        pollfd* pfds = new pollfd();
#endif

        int read_fds = socket->get_raw_socket();
        pfds->fd = read_fds;
        pfds->events = POLLIN;

        // exits after this time if no data has been received to check whether we should exit
        int timeout_ms = 100; 

#ifdef _WIN32
        if (WSAPoll(pfds, 1, timeout_ms) < 0) {
#else
        if (poll(pfds, 1, timeout_ms) < 0) {
#endif
            LOG_ERROR("poll(2) failed");
            if (pfds)
            {
                delete pfds;
                pfds = nullptr;
            }
            break;
        }

        if (pfds->revents & POLLIN) {

            // we write as many packets as socket has in the buffer
            while (!should_stop_)
            {
                // get the potential next write. To make sure processing doesn't start reading 
                // incomplete/old packets, we only update the index after buffer has the data
                int next_write_index = next_buffer_location(last_ring_write_index_);

                // create new buffer spaces if the process/read hasn't freed any spots on the ring buffer
                if (next_write_index == ring_read_index_)
                {
                    LOG_DEBUG("Reception buffer ran out, increasing the buffer size ...");

                    // increase the buffer size by 25%
                    int increase = ring_buffer_.size()/4;
                    if (increase == 0) // just so there is some increase
                        ++increase;

                    ring_mutex_.lock();
                    for (unsigned int i = 0; i < increase; ++i)
                    {
                        ring_buffer_.insert(ring_buffer_.begin() + next_write_index, { new uint8_t[RECV_BUFFER_SIZE] , 0 });
                        ++ring_read_index_;
                    }
                    ring_mutex_.unlock();
                }

                rtp_error_t ret = RTP_OK;
                // get the potential packet
                if ((ret = socket->recvfrom(ring_buffer_[next_write_index].data, RECV_BUFFER_SIZE,
                    MSG_DONTWAIT, &ring_buffer_[next_write_index].read)) == RTP_INTERRUPTED || 
                    ring_buffer_[next_write_index].read == 0)
                {
                    break;
                }
                else if (ret != RTP_OK) {
                    LOG_ERROR("recvfrom(2) failed! Reception flow cannot continue %d!", ret);
                    should_stop_ = true;
                    break;
                }

                // finally we update the ring buffer so processing (reading) knows that there is a new frame
                last_ring_write_index_ = next_write_index;
            }

            // start processing the packets by waking the processing thread
            process_cond_.notify_one();
        }

        if (pfds)
        {
            delete pfds;
            pfds = nullptr;
        }
    }
}

void uvgrtp::reception_flow::process_packet(int flags)
{
    LOG_DEBUG("Start processing loop");
    std::unique_lock<std::mutex> lk(wait_mtx_);

    while (!should_stop_)
    {
        // go to sleep waiting for something to process
        process_cond_.wait(lk); 

        if (should_stop_)
        {
            break;
        }

        ring_mutex_.lock();

        // process all available reads in one go
        while (next_buffer_location(ring_read_index_) != last_ring_write_index_)
        {
            // first update the read location
            ring_read_index_ = next_buffer_location(ring_read_index_);

            rtp_error_t ret = RTP_OK;

            // process the ring buffer location through all the handlers
            for (auto& handler : packet_handlers_) {
                uvgrtp::frame::rtp_frame* frame = nullptr;

                // Here we don't lock ring mutex because the chaging is only done above. 
                // NOTE: If there is a need for multiple processing threads, the read should be guarded
                switch ((ret = (*handler.second.primary)(ring_buffer_[ring_read_index_].read,
                    ring_buffer_[ring_read_index_].data, flags, &frame))) {
                    /* packet was handled successfully */
                case RTP_OK:
                    break;

                    /* packet was not handled by this primary handlers, proceed to the next one */
                case RTP_PKT_NOT_HANDLED:
                    continue;

                    /* packet was handled by the primary handler
                     * and should be dispatched to the auxiliary handler(s) */
                case RTP_PKT_MODIFIED:
                    this->call_aux_handlers(handler.first, flags, &frame);
                    break;

                case RTP_GENERIC_ERROR:
                    LOG_DEBUG("Received a corrupted packet!");
                    break;

                default:
                    LOG_ERROR("Unknown error code from packet handler: %d", ret);
                    break;
                }
            }
        }

        ring_mutex_.unlock();
    }
}

int uvgrtp::reception_flow::next_buffer_location(int current_location)
{
    return (current_location + 1) % ring_buffer_.size();
}