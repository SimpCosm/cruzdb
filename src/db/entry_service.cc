#include "db/entry_service.h"
#include <iostream>
#include "db/cruzdb.pb.h"

namespace cruzdb {

EntryService::EntryService(zlog::Log *log) :
  log_(log),
  stop_(false)
{
}

void EntryService::Start(uint64_t pos)
{
  pos_ = pos;
  log_reader_ = std::thread(&EntryService::Run, this);
  intention_reader_ = std::thread(&EntryService::IntentionReader, this);
}

void EntryService::Stop()
{
  {
    std::lock_guard<std::mutex> l(lock_);
    stop_ = true;
  }

  ai_matcher.shutdown();

  for (auto& q : intention_queues_) {
    q->Stop();
  }

  log_reader_.join();
  intention_reader_.join();
}

void EntryCache::Insert(std::unique_ptr<Intention> intention)
{
  auto pos = intention->Position();
  std::lock_guard<std::mutex> lk(lock_);
  if (intentions_.size() > 10) {
    intentions_.erase(intentions_.begin());
  }
  intentions_.emplace(pos, std::move(intention));
}

// obvs this is silly to return a copy. the cache should be storing shared
// pointers or something like this.
boost::optional<Intention> EntryCache::FindIntention(uint64_t pos)
{
  std::lock_guard<std::mutex> lk(lock_);
  auto it = intentions_.find(pos);
  if (it != intentions_.end()) {
    return *(it->second);
  }
  return boost::none;
}

int EntryService::AppendIntention(std::unique_ptr<Intention> intention,
    uint64_t *pos)
{
  const auto blob = intention->Serialize();
  int ret = log_->Append(blob, pos);
  if (ret == 0) {
    intention->SetPosition(*pos);
    cache_.Insert(std::move(intention));
  }
  return ret;
}

void EntryService::IntentionReader()
{
  uint64_t pos;
  boost::optional<uint64_t> last_min_pos;

  while (true) {
    std::unique_lock<std::mutex> lk(lock_);

    if (stop_)
      return;

    if (intention_queues_.empty()) {
      last_min_pos = boost::none;
      continue;
    }

    // min pos requested by any queue
    auto min_pos = intention_queues_.front()->Position();
    for (auto& q : intention_queues_) {
      min_pos = std::min(min_pos, q->Position());
    }

    lk.unlock();

    if (!last_min_pos) {
      last_min_pos = min_pos;
      pos = min_pos;
    }

    if (min_pos < *last_min_pos) {
      last_min_pos = boost::none;
      continue;
    }

    last_min_pos = min_pos;

    // the cache may know that this pos is not an intention, and that additional
    // slots in the log can be skipped over...
    auto intention = cache_.FindIntention(pos);
    if (intention) {
      lk.lock();
      for (auto& q : intention_queues_) {
        if (pos >= q->Position()) {
          q->Push(*intention);
        }
      }
      lk.unlock();

      pos++;
      continue;
    }

    // obvs this should be populating the cache too..
    std::string data;
    int ret = log_->Read(pos, &data);
    if (ret) {
      if (ret == -ENOENT) {
        continue;
      }
      assert(0);
    }

    cruzdb_proto::LogEntry entry;
    assert(entry.ParseFromString(data));
    assert(entry.IsInitialized());

    switch (entry.type()) {
      case cruzdb_proto::LogEntry::INTENTION:
        lk.lock();
        for (auto& q : intention_queues_) {
          if (pos >= q->Position()) {
            q->Push(Intention(entry.intention(), pos));
          }
        }
        lk.unlock();
        break;

      case cruzdb_proto::LogEntry::AFTER_IMAGE:
        break;

      default:
        assert(0);
        exit(1);
    }

    pos++;
  }
}

void EntryService::Run()
{
  while (true) {
    {
      std::lock_guard<std::mutex> l(lock_);
      if (stop_)
        return;
    }

    std::string data;

    // need to fill log positions. this is because it is important that any
    // after image that is currently the first occurence following its
    // intention, remains that way.
    int ret = log_->Read(pos_, &data);
    if (ret) {
      // TODO: be smart about reading. we shouldn't wait one second, and we
      // should sometimes fill holes. the current infrastructure won't generate
      // holes in testing, so this will work for now.
      if (ret == -ENOENT) {
        /*
         * TODO: currently we can run into a soft lockup where the log reader is
         * spinning on a position that hasn't been written. that's weird, since
         * we aren't observing any failed clients or sequencer or anything, so
         * every position should be written.
         *
         * what might be happening.. is that there is a hole, but the entity
         * assigned to fill that hole is waiting on something a bit further
         * ahead in the log, so no progress is being made...
         *
         * lets get a confirmation about the state here so we can record this
         * case. it would be an interesting case.
         *
         * do timeout waits so we can see with print statements...
         */
        continue;
      }
      assert(0);
    }

    cruzdb_proto::LogEntry entry;
    assert(entry.ParseFromString(data));
    assert(entry.IsInitialized());

    // TODO: look into using Arena allocation in protobufs, or moving to
    // flatbuffers. we basically want to avoid all the copying here, by doing
    // something like pushing a pointer onto these lists, or using move
    // semantics.
    switch (entry.type()) {
      case cruzdb_proto::LogEntry::AFTER_IMAGE:
        ai_matcher.push(entry.after_image(), pos_);
        break;

      case cruzdb_proto::LogEntry::INTENTION:
        break;

      default:
        assert(0);
        exit(1);
    }

    pos_++;
  }
}

EntryService::IntentionQueue *EntryService::NewIntentionQueue(uint64_t pos)
{
  auto queue = std::make_unique<IntentionQueue>(pos);
  auto ret = queue.get();
  std::lock_guard<std::mutex> lk(lock_);
  intention_queues_.emplace_back(std::move(queue));
  return ret;
}

EntryService::IntentionQueue::IntentionQueue(uint64_t pos) :
  pos_(pos),
  stop_(false)
{
}

void EntryService::IntentionQueue::Stop()
{
  {
    std::lock_guard<std::mutex> lk(lock_);
    stop_ = true;
  }
  cond_.notify_all();
}

boost::optional<Intention> EntryService::IntentionQueue::Wait()
{
  std::unique_lock<std::mutex> lk(lock_);
  cond_.wait(lk, [this] { return !q_.empty() || stop_; });
  if (stop_) {
    return boost::none;
  }
  assert(!q_.empty());
  auto i = q_.front();
  q_.pop();
  return i;
}

uint64_t EntryService::IntentionQueue::Position() const
{
  std::lock_guard<std::mutex> lk(lock_);
  return pos_;
}

void EntryService::IntentionQueue::Push(Intention intention)
{
  std::lock_guard<std::mutex> lk(lock_);
  assert(pos_ <= intention.Position());
  pos_ = intention.Position() + 1;
  q_.emplace(intention);
  cond_.notify_one();
}

std::list<Intention>
EntryService::ReadIntentions(std::vector<uint64_t> addrs)
{
  std::list<Intention> intentions;
  assert(!addrs.empty());
  for (auto pos : addrs) {
    std::string data;
    int ret = log_->Read(pos, &data);
    assert(ret == 0);
    cruzdb_proto::LogEntry entry;
    assert(entry.ParseFromString(data));
    assert(entry.IsInitialized());
    assert(entry.type() == cruzdb_proto::LogEntry::INTENTION);
    intentions.emplace_back(Intention(entry.intention(), pos));
  }
  return intentions;
}

EntryService::PrimaryAfterImageMatcher::PrimaryAfterImageMatcher() :
  shutdown_(false),
  matched_watermark_(0)
{
}

void EntryService::PrimaryAfterImageMatcher::watch(
    std::vector<SharedNodeRef> delta,
    std::unique_ptr<PersistentTree> intention)
{
  std::lock_guard<std::mutex> lk(lock_);

  const auto ipos = intention->Intention();

  auto it = afterimages_.find(ipos);
  if (it == afterimages_.end()) {
    afterimages_.emplace(ipos,
        PrimaryAfterImage{boost::none,
        std::move(intention),
        std::move(delta)});
  } else {
    assert(it->second.pos);
    assert(!it->second.tree);
    intention->SetAfterImage(*it->second.pos);
    it->second.pos = boost::none;
    matched_.emplace_back(std::make_pair(std::move(delta),
        std::move(intention)));
    cond_.notify_one();
  }

  gc();
}

void EntryService::PrimaryAfterImageMatcher::push(
    const cruzdb_proto::AfterImage& ai, uint64_t pos)
{
  std::lock_guard<std::mutex> lk(lock_);

  const auto ipos = ai.intention();
  if (ipos <= matched_watermark_) {
    return;
  }

  auto it = afterimages_.find(ipos);
  if (it == afterimages_.end()) {
    afterimages_.emplace(ipos, PrimaryAfterImage{pos, nullptr, {}});
  } else if (!it->second.pos && it->second.tree) {
    assert(it->second.tree->Intention() == ipos);
    it->second.tree->SetAfterImage(pos);
    matched_.emplace_back(std::make_pair(std::move(it->second.delta),
        std::move(it->second.tree)));
    cond_.notify_one();
  }

  gc();
}

std::pair<std::vector<SharedNodeRef>,
  std::unique_ptr<PersistentTree>>
EntryService::PrimaryAfterImageMatcher::match()
{
  std::unique_lock<std::mutex> lk(lock_);

  cond_.wait(lk, [&] { return !matched_.empty() || shutdown_; });

  if (shutdown_) {
    return std::make_pair(std::vector<SharedNodeRef>(), nullptr);
  }

  assert(!matched_.empty());

  auto tree = std::move(matched_.front());
  matched_.pop_front();

  return std::move(tree);
}

void EntryService::PrimaryAfterImageMatcher::shutdown()
{
  std::lock_guard<std::mutex> l(lock_);
  shutdown_ = true;
  cond_.notify_one();
}

void EntryService::PrimaryAfterImageMatcher::gc()
{
  auto it = afterimages_.begin();
  while (it != afterimages_.end()) {
    auto ipos = it->first;
    assert(matched_watermark_ < ipos);
    auto& pai = it->second;
    if (!pai.pos && !pai.tree) {
      matched_watermark_ = ipos;
      it = afterimages_.erase(it);
    } else {
      // as long as the watermark is positioned such that no unmatched intention
      // less than the water is in the index, then gc could move forward and
      // continue removing matched entries.
      break;
    }
  }
}

void EntryService::AppendAfterImageAsync(const std::string& blob)
{
  uint64_t afterimage_pos;
  int ret = log_->Append(blob, &afterimage_pos);
  assert(ret == 0);
}

uint64_t EntryService::CheckTail()
{
  uint64_t pos;
  int ret = log_->CheckTail(&pos);
  if (ret) {
    std::cerr << "failed to check tail" << std::endl;
    assert(0);
    exit(1);
  }
  return pos;
}

uint64_t EntryService::Append(cruzdb_proto::Intention& intention)
{
  cruzdb_proto::LogEntry entry;
  entry.set_type(cruzdb_proto::LogEntry::INTENTION);
  entry.set_allocated_intention(&intention);
  assert(entry.IsInitialized());

  std::string blob;
  assert(entry.SerializeToString(&blob));
  entry.release_intention();

  uint64_t pos;
  int ret = log_->Append(blob, &pos);
  if (ret) {
    std::cerr << "failed to append intention" << std::endl;
    assert(0);
    exit(1);
  }
  return pos;
}

uint64_t EntryService::Append(cruzdb_proto::AfterImage& after_image)
{
  cruzdb_proto::LogEntry entry;
  entry.set_type(cruzdb_proto::LogEntry::AFTER_IMAGE);
  entry.set_allocated_after_image(&after_image);
  assert(entry.IsInitialized());

  std::string blob;
  assert(entry.SerializeToString(&blob));
  entry.release_after_image();

  uint64_t pos;
  int ret = log_->Append(blob, &pos);
  if (ret) {
    std::cerr << "failed to append after image" << std::endl;
    assert(0);
    exit(1);
  }
  return pos;
}

}
