#include "node_cache.h"
#include "db_impl.h"
#include <time.h>
#include <deque>
#include <condition_variable>

namespace cruzdb {

// TODO: cache should enforce properties like all nodes have physical
// addresses...

// TODO: if usage goes above high marker block new txns
static const size_t low_marker  =  128*1024*1024;
//static const size_t high_marker = 8*1024*1024;

void NodeCache::do_vaccum_()
{
  while (true) {
    std::unique_lock<std::mutex> l(lock_);

    cond_.wait(l, [this]{
        return !traces_.empty() || UsedBytes() > low_marker || stop_;
    });

    if (stop_)
      return;

    std::list<std::vector<NodeAddress>> traces;
    traces_.swap(traces_);

    l.unlock();

    // apply lru updates
    for (auto trace : traces) {
      for (auto address : trace) {

        // TODO: we'll need to figure out a solution for converting addresses or
        // using another cache index. we don't want to take a lock for every
        // conversion. at the moment, it doesn't really matter: we can drop
        // anything from the cache and the system must still run correctly.
        auto key = std::make_pair<int64_t, int>(
            address.Position(), address.Offset());

        auto slot = pair_hash()(key) % num_slots_;
        auto& shard = shards_[slot];
        auto& nodes_ = shard->nodes;
        auto& nodes_lru_ = shard->lru;

        std::unique_lock<std::mutex> lk(shard->lock);

        auto node_it = nodes_.find(key);
        if (node_it == nodes_.end())
          continue;
        entry& e = node_it->second;
        nodes_lru_.erase(e.lru_iter);
        nodes_lru_.emplace_front(key);
        e.lru_iter = nodes_lru_.begin();
      }
    }

    if (UsedBytes() > low_marker) {
      ssize_t target_bytes = (UsedBytes() - low_marker) / num_slots_;
      for (size_t slot = 0; slot < num_slots_; slot++) {
        auto& shard = shards_[slot];
        auto& nodes_ = shard->nodes;
        auto& nodes_lru_ = shard->lru;

        std::unique_lock<std::mutex> lk(shard->lock);

        ssize_t left = target_bytes;
        while (left > 0) {
          if (nodes_.empty())
            break;
          auto key = nodes_lru_.back();
          auto nit = nodes_.find(key);
          assert(nit != nodes_.end());
          used_bytes_ -= nit->second.node->ByteSize();
          left -= nit->second.node->ByteSize();
          nodes_.erase(nit);
          nodes_lru_.pop_back();
        }
      }
    }
  }
}

// when resolving a node we only resolve the single node. figuring out when to
// resolve an entire intention would be interesting.
SharedNodeRef NodeCache::fetch(std::vector<NodeAddress>& trace,
    boost::optional<NodeAddress>& address)
{
  auto offset = address->Offset();
  auto csn = address->Position();
  if (!address->IsAfterImage()) {
    csn = IntentionToAfterImage(csn);
  }
  auto key = std::make_pair(csn, offset);

  auto slot = pair_hash()(key) % num_slots_;
  auto& shard = shards_[slot];
  auto& nodes_ = shard->nodes;
  auto& nodes_lru_ = shard->lru;

  std::unique_lock<std::mutex> lk(shard->lock);

  // is the node in the cache?
  auto it = nodes_.find(key);
  if (it != nodes_.end()) {
    entry& e = it->second;
    nodes_lru_.erase(e.lru_iter);
    nodes_lru_.emplace_front(key);
    e.lru_iter = nodes_lru_.begin();
    return e.node;
  }

  // release lock for I/O
  lk.unlock();

  // publish the lru traces. we are doing this here because if the log read
  // blocks or takes a long time we don't want to reduce the quality of the
  // trace by having it be outdated. how important is this? is it over
  // optimization?
  {
    std::lock_guard<std::mutex> l(lock_);
    if (!trace.empty()) {
      traces_.emplace_front();
      traces_.front().swap(trace);
      cond_.notify_one();
    }
  }

  std::string snapshot;
  int ret = log_->Read(csn, &snapshot);
  assert(ret == 0);

  cruzdb_proto::LogEntry log_entry;
  assert(log_entry.ParseFromString(snapshot));
  assert(log_entry.IsInitialized());
  assert(log_entry.type() != cruzdb_proto::LogEntry::INTENTION);
  assert(log_entry.type() == cruzdb_proto::LogEntry::AFTER_IMAGE);
  auto i = log_entry.after_image();

  auto nn = deserialize_node(i, csn, offset);

  // add to cache. make sure it didn't show up after we released the lock to
  // read the node from the log.
  lk.lock();

  it = nodes_.find(key);
  if (it != nodes_.end()) {
    entry& e = it->second;
    nodes_lru_.erase(e.lru_iter);
    nodes_lru_.emplace_front(key);
    e.lru_iter = nodes_lru_.begin();
    return e.node;
  }

  nodes_lru_.emplace_front(key);
  auto iter = nodes_lru_.begin();
  auto res = nodes_.insert(
      std::make_pair(key, entry{nn, iter}));
  assert(res.second);

  used_bytes_ += nn->ByteSize();

  return nn;
}

// disabling resolution during node deserialization because currently when
// this is called we are holding a lock on a particular cache shard. allowing
// this would require us to take multiple locks at a time (deal with
// deadlock by ordering acquires etc...) or promote this resolution to a
// higher level in the call stack where we could iterate over the new nodes
// and acquire each shard lock without other locks. we'll just come back to
// this optimization later.
//
//void NodeCache::ResolveNodePtr(NodePtr& ptr)
//{
//  auto key = std::make_pair(ptr.csn(), ptr.offset());
//
//  auto slot = pair_hash()(key) % num_slots_;
//  auto& shard = shards_[slot];
//  auto& nodes_ = shard->nodes;
//  auto& nodes_lru_ = shard->lru;
//
//  auto node_it = nodes_.find(key);
//  if (node_it == nodes_.end())
//    return;
//
//  // lru update
//  entry& e = node_it->second;
//  nodes_lru_.erase(e.lru_iter);
//  nodes_lru_.emplace_front(key);
//  e.lru_iter = nodes_lru_.begin();
//
//  ptr.set_ref(e.node);
//}

NodePtr NodeCache::CacheAfterImage(const cruzdb_proto::AfterImage& i,
    uint64_t pos)
{
  if (i.tree_size() == 0) {
    NodePtr ret(Node::Nil(), nullptr);
    return ret;
  }

  int idx;
  SharedNodeRef nn = nullptr;
  for (idx = 0; idx < i.tree_size(); idx++) {

    // no locking on deserialize_node is OK
    nn = deserialize_node(i, pos, idx);

    auto key = std::make_pair(pos, idx);

    auto slot = pair_hash()(key) % num_slots_;
    auto& shard = shards_[slot];
    auto& nodes_ = shard->nodes;
    auto& nodes_lru_ = shard->lru;

    std::unique_lock<std::mutex> lk(shard->lock);

    auto it = nodes_.find(key);
    if (it != nodes_.end()) {
      // if this was the last node, then make sure when we fall through to the
      // end of the routine that nn points to this node instead of the one
      // that was constructed above.
      nn = it->second.node;
      continue;
    }

    nodes_lru_.emplace_front(key);
    auto iter = nodes_lru_.begin();
    auto res = nodes_.insert(
        std::make_pair(key, entry{nn, iter}));
    assert(res.second);

    used_bytes_ += nn->ByteSize();
  }

  assert(nn != nullptr);
  NodePtr ret(nn, db_);
  ret.SetAfterImageAddress(pos, idx - 1);
  return ret;
}

SharedNodeRef NodeCache::deserialize_node(const cruzdb_proto::AfterImage& i,
    uint64_t pos, int index) const
{
  const cruzdb_proto::Node& n = i.tree(index);

  auto nn = std::make_shared<Node>(n.key(), n.val(), n.red(),
      nullptr, nullptr, i.intention(), false, db_);

  if (!n.left().nil()) {
    uint64_t position;
    uint16_t offset = n.left().off();
    if (n.left().self()) {
      position = pos;
    } else {
      position = n.left().csn();
    }
    nn->left.SetAfterImageAddress(position, offset);
  } else {
    nn->left.set_ref(Node::Nil());
  }

  if (!n.right().nil()) {
    uint64_t position;
    uint16_t offset = n.right().off();
    if (n.right().self()) {
      position = pos;
    } else {
      position = n.right().csn();
    }
    nn->right.SetAfterImageAddress(position, offset);
  } else {
    nn->right.set_ref(Node::Nil());
  }

  return nn;
}

NodePtr NodeCache::ApplyAfterImageDelta(
    const std::vector<SharedNodeRef>& delta,
    uint64_t after_image_pos)
{
  if (delta.empty()) {
    NodePtr ret(Node::Nil(), nullptr);
    return ret;
  }

  int offset = 0;
  for (auto nn : delta) {
    nn->set_read_only();

    auto key = std::make_pair(after_image_pos, offset);

    auto slot = pair_hash()(key) % num_slots_;
    auto& shard = shards_[slot];
    auto& nodes_ = shard->nodes;
    auto& nodes_lru_ = shard->lru;

    std::unique_lock<std::mutex> lk(shard->lock);

    nodes_lru_.emplace_front(key);
    auto iter = nodes_lru_.begin();
    auto res = nodes_.insert(
        std::make_pair(key, entry{nn, iter}));
    assert(res.second);
    offset++;

    used_bytes_ += nn->ByteSize();
  }

  auto root = delta.back();
  NodePtr ret(root, db_);
  ret.SetAfterImageAddress(after_image_pos, offset - 1);
  return ret;
}

}