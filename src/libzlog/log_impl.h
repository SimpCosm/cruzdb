#pragma once
#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>
#include "include/zlog/log.h"
#include "libseq/libseqr.h"
#include "include/zlog/backend.h"
#include "striper.h"

#define DEFAULT_STRIPE_SIZE 100

namespace zlog {

typedef Backend *(*backend_allocate_t)(void);
typedef void (*backend_release_t)(Backend*);

class LogImpl : public Log {
 public:
  LogImpl(const LogImpl&) = delete;
  LogImpl(const LogImpl&&) = delete;
  LogImpl& operator=(const LogImpl&) = delete;
  LogImpl& operator=(const LogImpl&&) = delete;

  LogImpl(std::shared_ptr<Backend> backend,
      const std::string& name,
      const std::string& hoid,
      const std::string& prefix) :
    shutdown(false),
    backend(backend),
    sequencer(nullptr),
    name(name),
    hoid(hoid),
    striper(prefix)
  {
    view_update_thread = std::thread(&LogImpl::ViewUpdater, this);
  }

  ~LogImpl();

 public:
  void ViewUpdater();
  int UpdateView();

  int CreateNextView(uint64_t *pepoch, uint64_t *pmaxpos, bool *pempty,
      zlog_proto::View& view);
  int ProposeNextView(uint64_t next_epoch, const zlog_proto::View& view);
  int CreateCut(uint64_t *pepoch, uint64_t *pmaxpos, bool *pempty);
  int Seal(const std::vector<std::string>& objects,
      uint64_t epoch, uint64_t *pmaxpos, bool *pempty);
  int ProposeSharedMode();
  int ProposeExclusiveMode();

  static int Open(const std::string& scheme, const std::string& name,
      const std::map<std::string, std::string>& opts, LogImpl **logpp,
      std::shared_ptr<Backend> *out_backend);

 public:
  int CheckTail(uint64_t *pposition) override;
  int CheckTail(uint64_t *pposition, uint64_t *epoch, bool increment);

#ifdef STREAMING_SUPPORT
  /*
   * When next == true
   *   - position: new log tail
   *   - stream_backpointers: back pointers for each stream in stream_ids
   *
   * When next == false
   *   - position: current log tail
   *   - stream_backpointers: back pointers for each stream in stream_ids
   */
  int CheckTail(const std::set<uint64_t>& stream_ids,
      std::map<uint64_t, std::vector<uint64_t>>& stream_backpointers,
      uint64_t *position, bool next);
#endif

 public:
  int Read(uint64_t position, std::string *data) override;
  int Read(uint64_t epoch, uint64_t position, std::string *data);

  int Append(const Slice& data, uint64_t *pposition = NULL) override;

  int Fill(uint64_t position) override;
  int Fill(uint64_t epoch, uint64_t position);

  int Trim(uint64_t position) override;

 public:
  int AioRead(uint64_t position, zlog::AioCompletion *c,
      std::string *datap) override;

  int AioAppend(zlog::AioCompletion *c, const Slice& data,
      uint64_t *pposition = NULL) override;

#ifdef STREAMING_SUPPORT
 public:
  int OpenStream(uint64_t stream_id, zlog::Stream **streamptr) override;
  int StreamMembership(std::set<uint64_t>& stream_ids, uint64_t position) override;
  int StreamMembership(uint64_t epoch, std::set<uint64_t>& stream_ids, uint64_t position);
  int StreamHeader(const std::string& data, std::set<uint64_t>& stream_ids,
      size_t *header_size = NULL);
  int MultiAppend(const Slice& data, const std::set<uint64_t>& stream_ids,
      uint64_t *pposition = NULL) override;
#endif

 public:
  int StripeWidth() override {
    return striper.GetCurrent().width;
  }

 public:
  bool shutdown;
  std::mutex lock;

  // thread-safe
  std::shared_ptr<Backend> backend;

  std::shared_ptr<SeqrClient> sequencer;
  const std::string name;
  const std::string hoid;

  // thread-safe
  Striper striper;

  std::string exclusive_cookie;
  uint64_t exclusive_position;
  bool exclusive_empty;

  std::condition_variable view_update;
  std::list<std::pair<std::condition_variable*, bool*>> view_update_waiters;
  std::thread view_update_thread;
};

}
