#include "persistent_tree.h"
#include "db_impl.h"

namespace cruzdb {

void PersistentTree::UpdateLRU()
{
  db_->UpdateLRU(trace_);
}


// when a node is copied, its left and right pointers are also copied. after
// having copied a node, if one of the child pointers turns out to point to a
// another new node in the delta, then the physical address needs to be updated
// to point to self. that means that the node can temporarily be in a weird
// state. it would be good to work on eliminating that special state. this whole
// thing started because in SetIntentionAddress i asserted that no address would
// exist since its pointing at a new node, but that isn't always the case.
void PersistentTree::infect_node_ptr(uint64_t intention, NodePtr& src, int maybe_offset)
{
  if (src.ref_notrace() != Node::Nil() &&
      src.ref(trace_)->rid() == rid_) {
    src.SetIntentionAddress(intention, maybe_offset);
  }
}

void PersistentTree::infect_node(SharedNodeRef node,
    uint64_t intention, int maybe_left_offset, int maybe_right_offset)
{
  infect_node_ptr(intention, node->left, maybe_left_offset);
  infect_node_ptr(intention, node->right, maybe_right_offset);
}

void PersistentTree::infect_after_image(SharedNodeRef node, uint64_t intention, int& field_index)
{
  assert(node != nullptr);

  if (node == Node::Nil() || node->rid() != rid_)
    return;

  infect_after_image(node->left.ref(trace_), intention, field_index);
  auto maybe_left_offset = field_index - 1;

  infect_after_image(node->right.ref(trace_), intention, field_index);
  auto maybe_right_offset = field_index - 1;

  infect_node(node, intention, maybe_left_offset, maybe_right_offset);
  field_index++;
}

boost::optional<int> PersistentTree::infect_self_pointers(uint64_t intention,
    bool expect_intention_rid)
{
  // before infection, the rid of the nodes and this tree are switched over to
  // the intention value. the effect of this should be the same as if the tree
  // had originally been initialized with intention as the rid.
  //
  // when serial intentions are able to re-use in-memory trees from local
  // transactions, those trees have a negative rid value. however, normal
  // intention replay produces trees with an rid equal to the position of the
  // intention. this normalizes it to make sure that everything is identical.
  //
  // is this really necessary? probably not. as it stands, the rid just needs to
  // be unique in memory. however, when nodes are freed from memory and restored
  // from the log, they receive an rid equal to their intention. that makes all
  // but the nodes that are converted from in-memory transactions in the serial
  // case have unified rids.
  //
  // what happens when nodes that are part of the same delta have different
  // rids? nothing, as long as there is no process that wants to find the nodes
  // that are part of the same delta, and in the current system, that doesn't
  // happen except for during serialization, but we keep nodes in memory until
  // then so there is no risk.
  //
  // as it stands, updating the value is a cheap process because have to look at
  // all the nodes anyway which makes the decision easy. but... it's something
  // to think about in the future because in the fast path of the transaction
  // processor, it may be the case that we are eventually looking for ways to
  // reduce any bit of work.
  for (auto& n : fresh_nodes_) {
    if (expect_intention_rid) {
      assert(n->rid() == (int64_t)intention);
    } else {
      assert(n->rid() < 0);
    }
    n->set_rid(intention);
  }
  if (expect_intention_rid) {
    assert(rid_ == (int64_t)intention);
  } else {
    assert(rid_ < 0);
  }
  rid_ = (int64_t)intention;

  if (root_ == nullptr) {
    root_ = Node::Copy(src_root_.ref_notrace(), db_, rid_);
    return boost::none;
  }

  assert(root_ == Node::Nil() ||
      root_->rid() == rid_);

  int field_index = 0;
  infect_after_image(root_, intention, field_index);

  assert(field_index > 0);
  return field_index - 1;
}

void PersistentTree::serialize_node_ptr(cruzdb_proto::NodePtr *dst,
    NodePtr& src, int maybe_offset)
{
  if (src.ref(trace_) == Node::Nil()) {
    dst->set_nil(true);
    dst->set_self(false);
  } else if (src.ref(trace_)->rid() == rid_) {
    dst->set_nil(false);
    dst->set_self(true);
    dst->set_off(maybe_offset);
    // assert the offset was set correctly during infection.
    assert(src.Address());
    assert(src.Address()->Offset() == maybe_offset);
  } else {
    auto address = src.Address();
    assert(address);

    assert(src.ref(trace_) != nullptr);
    dst->set_nil(false);
    dst->set_self(false);

    if (address->IsAfterImage()) {
      dst->set_afterimage(address->Position());
    } else {
      const auto i_pos = address->Position();
      const auto ai_pos = db_->IntentionToAfterImage(i_pos);
      if (ai_pos) {
        dst->set_afterimage(*ai_pos);
      } else {
        dst->set_intention(i_pos);
      }
    }

    dst->set_off(address->Offset());
  }
}

void PersistentTree::serialize_node(cruzdb_proto::Node *dst,
    SharedNodeRef node, int maybe_left_offset, int maybe_right_offset)
{
  dst->set_red(node->red());
  dst->set_key(node->key().ToString());
  dst->set_val(node->val().ToString());

  serialize_node_ptr(dst->mutable_left(), node->left, maybe_left_offset);
  serialize_node_ptr(dst->mutable_right(), node->right, maybe_right_offset);
}

void PersistentTree::serialize_intention(cruzdb_proto::AfterImage& i,
    SharedNodeRef node, int& field_index, std::vector<SharedNodeRef>& delta)
{
  assert(node != nullptr);

  if (node == Node::Nil() || node->rid() != rid_)
    return;

  // serialize the left side of the tree. after the call returns,
  // maybe_left_offset will contain the offset of the last node that was
  // serialized. if the node is non-nil and is a new node in the afterimage,
  // then maybe_left_offset is valid (its validity is checked in
  // serialize_node_ptr).
  serialize_intention(i, node->left.ref(trace_), field_index, delta);
  auto maybe_left_offset = field_index - 1;

  serialize_intention(i, node->right.ref(trace_), field_index, delta);
  auto maybe_right_offset = field_index - 1;

  // new serialized node in the intention
  cruzdb_proto::Node *dst = i.add_tree();
  serialize_node(dst, node, maybe_left_offset, maybe_right_offset);
  delta.push_back(node);
  field_index++;
}

void PersistentTree::SerializeAfterImage(cruzdb_proto::AfterImage& i,
    uint64_t intention,
    std::vector<SharedNodeRef>& delta)
{
  int field_index = 0;
  assert(root_ != nullptr);
  if (root_ == Node::Nil()) {
    // ???
  } else
    assert(root_->rid() == rid_);

  serialize_intention(i, root_, field_index, delta);

  // only valid when the transaction is being used to produce after images when
  // processing intentions from the log.
  i.set_intention(intention);
}

void PersistentTree::SetDeltaPosition(std::vector<SharedNodeRef>& delta,
    uint64_t pos)
{
  for (const auto nn : delta) {
    if (nn->left.ref_notrace()->rid() == rid_) {
      nn->left.ConvertToAfterImage(pos);
    }
    if (nn->right.ref_notrace()->rid() == rid_) {
      nn->right.ConvertToAfterImage(pos);
    }
  }
}


SharedNodeRef PersistentTree::insert_recursive(std::deque<SharedNodeRef>& path,
    const zlog::Slice& key, const zlog::Slice& value, const SharedNodeRef& node)
{
  assert(node != nullptr);

  if (node == Node::Nil()) {
    auto nn = std::make_shared<Node>(key, value, true, Node::Nil(),
        Node::Nil(), rid_, false, db_);
    path.push_back(nn);
    fresh_nodes_.push_back(nn);
    return nn;
  }

  int cmp = key.compare(zlog::Slice(node->key().data(),
        node->key().size()));
  bool less = cmp < 0;
  bool equal = cmp == 0;

  /*
   * How should we handle key/value updates? What about when the values are
   * the same?
   */
  if (equal)
    return nullptr;

  auto child = insert_recursive(path, key, value,
      (less ? node->left.ref(trace_) : node->right.ref(trace_)));

  if (child == nullptr)
    return child;

  /*
   * the copy_node operation will copy the child node references, as well as
   * the csn/offset for each child node reference. however below the reference
   * is updated without updating the csn/offset, which are fixed later when
   * the intention is build.
   */
  SharedNodeRef copy;
  if (node->rid() == rid_)
    copy = node;
  else {
    copy = Node::Copy(node, db_, rid_);
    fresh_nodes_.push_back(copy);
  }

  if (less)
    copy->left.set_ref(child);
  else
    copy->right.set_ref(child);

  path.push_back(copy);

  return copy;
}

template<typename ChildA, typename ChildB >
SharedNodeRef PersistentTree::rotate(SharedNodeRef parent,
    SharedNodeRef child, ChildA child_a, ChildB child_b, SharedNodeRef& root)
{
  // copy over ref and csn/off because we might be moving a pointer that
  // points outside of the current intentino.
  NodePtr grand_child = child_b(child); // copy constructor makes grand_child read-only
  child_b(child) = child_a(grand_child.ref(trace_));

  if (root == child) {
    root = grand_child.ref(trace_);
  } else if (child_a(parent).ref(trace_) == child)
    child_a(parent) = grand_child;
  else
    child_b(parent) = grand_child;

  // we do not update csn/off here because child is always a pointer to a node
  // in the current intention so its csn/off will be updated during intention
  // serialization step.
  assert(child->rid() == rid_);
  child_a(grand_child.ref(trace_)).set_ref(child);

  return grand_child.ref(trace_);
}

template<typename ChildA, typename ChildB>
void PersistentTree::insert_balance(SharedNodeRef& parent, SharedNodeRef& nn,
    std::deque<SharedNodeRef>& path, ChildA child_a, ChildB child_b,
    SharedNodeRef& root)
{
  assert(path.front() != Node::Nil());
  NodePtr& uncle = child_b(path.front());
  if (uncle.ref(trace_)->red()) {
    if (uncle.ref(trace_)->rid() != rid_) {
      auto n = Node::Copy(uncle.ref(trace_), db_, rid_);
      fresh_nodes_.push_back(n);
      uncle.set_ref(n);
    }
    parent->set_red(false);
    uncle.ref(trace_)->set_red(false);
    path.front()->set_red(true);
    nn = pop_front(path);
    parent = pop_front(path);
  } else {
    if (nn == child_b(parent).ref(trace_)) {
      std::swap(nn, parent);
      rotate(path.front(), nn, child_a, child_b, root);
    }
    auto grand_parent = pop_front(path);
    grand_parent->swap_color(parent);
    rotate(path.front(), grand_parent, child_b, child_a, root);
  }
}

SharedNodeRef PersistentTree::delete_recursive(std::deque<SharedNodeRef>& path,
    const zlog::Slice& key, const SharedNodeRef& node)
{
  assert(node != nullptr);

  if (node == Node::Nil()) {
    return nullptr;
  }

  int cmp = key.compare(zlog::Slice(node->key().data(),
        node->key().size()));
  bool less = cmp < 0;
  bool equal = cmp == 0;

  if (equal) {
    SharedNodeRef copy;
    if (node->rid() == rid_)
      copy = node;
    else {
      copy = Node::Copy(node, db_, rid_);
      fresh_nodes_.push_back(copy);
    }
    path.push_back(copy);
    return copy;
  }

  auto child = delete_recursive(path, key,
      (less ? node->left.ref(trace_) : node->right.ref(trace_)));

  if (child == nullptr) {
    return child;
  }

  /*
   * the copy_node operation will copy the child node references, as well as
   * the csn/offset for each child node reference. however below the reference
   * is updated without updating the csn/offset, which are fixed later when
   * the intention is build.
   */
  SharedNodeRef copy;
  if (node->rid() == rid_)
    copy = node;
  else {
    copy = Node::Copy(node, db_, rid_);
    fresh_nodes_.push_back(copy);
  }

  if (less)
    copy->left.set_ref(child);
  else
    copy->right.set_ref(child);

  path.push_back(copy);

  return copy;
}

void PersistentTree::transplant(SharedNodeRef parent, SharedNodeRef removed,
    SharedNodeRef transplanted, SharedNodeRef& root)
{
  if (parent == Node::Nil()) {
    root = transplanted;
  } else if (parent->left.ref(trace_) == removed) {
    parent->left.set_ref(transplanted);
  } else {
    parent->right.set_ref(transplanted);
  }
}

SharedNodeRef PersistentTree::build_min_path(SharedNodeRef node, std::deque<SharedNodeRef>& path)
{
  assert(node != nullptr);
  assert(node->left.ref(trace_) != nullptr);
  while (node->left.ref(trace_) != Node::Nil()) {
    assert(node->left.ref(trace_) != nullptr);
    if (node->left.ref(trace_)->rid() != rid_) {
      auto n = Node::Copy(node->left.ref(trace_), db_, rid_);
      fresh_nodes_.push_back(n);
      node->left.set_ref(n);
    }
    path.push_front(node);
    node = node->left.ref(trace_);
    assert(node != nullptr);
  }
  return node;
}

template<typename ChildA, typename ChildB>
void PersistentTree::mirror_remove_balance(SharedNodeRef& extra_black, SharedNodeRef& parent,
    std::deque<SharedNodeRef>& path, ChildA child_a, ChildB child_b, SharedNodeRef& root)
{
  SharedNodeRef brother = child_b(parent).ref(trace_);

  if (brother->red()) {
    if (brother->rid() != rid_) {
      auto n = Node::Copy(brother, db_, rid_);
      fresh_nodes_.push_back(n);
      child_b(parent).set_ref(n);
    } else
      child_b(parent).set_ref(brother);
    brother = child_b(parent).ref(trace_);

    brother->swap_color(parent);
    rotate(path.front(), parent, child_a, child_b, root);
    path.push_front(brother);

    brother = child_b(parent).ref(trace_);
  }

  assert(brother != nullptr);

  assert(brother->left.ref(trace_) != nullptr);
  assert(brother->right.ref(trace_) != nullptr);

  if (!brother->left.ref(trace_)->red() && !brother->right.ref(trace_)->red()) {
    if (brother->rid() != rid_) {
      auto n = Node::Copy(brother, db_, rid_);
      fresh_nodes_.push_back(n);
      child_b(parent).set_ref(n);
    } else
      child_b(parent).set_ref(brother);
    brother = child_b(parent).ref(trace_);

    brother->set_red(true);
    extra_black = parent;
    parent = pop_front(path);
  } else {
    if (!child_b(brother).ref(trace_)->red()) {
      if (brother->rid() != rid_) {
        auto n = Node::Copy(brother, db_, rid_);
        fresh_nodes_.push_back(n);
        child_b(parent).set_ref(n);
      } else
        child_b(parent).set_ref(brother);
      brother = child_b(parent).ref(trace_);

      if (child_a(brother).ref(trace_)->rid() != rid_) {
        auto n = Node::Copy(child_a(brother).ref(trace_), db_, rid_);
        fresh_nodes_.push_back(n);
        child_a(brother).set_ref(n);
      }
      brother->swap_color(child_a(brother).ref(trace_));
      brother = rotate(parent, brother, child_b, child_a, root);
    }

    if (brother->rid() != rid_) {
      auto n = Node::Copy(brother, db_, rid_);
      fresh_nodes_.push_back(n);
      child_b(parent).set_ref(n);
    } else
      child_b(parent).set_ref(brother);
    brother = child_b(parent).ref(trace_);

    if (child_b(brother).ref(trace_)->rid() != rid_) {
      auto n = Node::Copy(child_b(brother).ref(trace_), db_, rid_);
      fresh_nodes_.push_back(n);
      child_b(brother).set_ref(n);
    }
    brother->set_red(parent->red());
    parent->set_red(false);
    child_b(brother).ref(trace_)->set_red(false);
    rotate(path.front(), parent, child_a, child_b, root);

    extra_black = root;
    parent = Node::Nil();
  }
}

void PersistentTree::balance_delete(SharedNodeRef extra_black,
    std::deque<SharedNodeRef>& path, SharedNodeRef& root)
{
  auto parent = pop_front(path);

  assert(extra_black != nullptr);
  assert(root != nullptr);
  assert(parent != nullptr);

  //db_->cache_.ResolveNodePtr(parent->left);
  //assert(parent->left.ref() != nullptr);

  while (extra_black != root && !extra_black->red()) {
    if (parent->left.ref(trace_) == extra_black)
      mirror_remove_balance(extra_black, parent, path, left, right, root);
    else
      mirror_remove_balance(extra_black, parent, path, right, left, root);
  }

  SharedNodeRef new_node;
  if (extra_black->rid() == rid_)
    new_node = extra_black;
  else {
    new_node = Node::Copy(extra_black, db_, rid_);
    fresh_nodes_.push_back(new_node);
  }
  transplant(parent, extra_black, new_node, root);

  /*
   * new_node may point to nil, and this call sets the color to black (nil is
   * always black, so this is OK). however we treat nil as read-only, so we
   * only make this call that may throw a non-read-only assertion failure for
   * non-nil nodes.
   *
   * TODO: is there something fundmentally wrong with the algorithm that
   * new_node may even point to nil?
   */
  if (new_node != Node::Nil())
    new_node->set_red(false);
}

SharedNodeRef PersistentTree::copy_recursive(const zlog::Slice& key,
    const SharedNodeRef& node)
{
  assert(node != nullptr);

  // not found
  if (node == Node::Nil())
    return nullptr;

  int cmp = key.compare(zlog::Slice(node->key().data(),
        node->key().size()));
  bool less = cmp < 0;
  bool equal = cmp == 0;

  if (equal) {
    // was already copied?
    if (node->rid() == rid_) {
      return nullptr;
    }
    auto copy = Node::Copy(node, db_, rid_);
    fresh_nodes_.push_back(copy);
    return copy;
  }

  auto child = copy_recursive(key,
      (less ? node->left.ref(trace_) : node->right.ref(trace_)));

  if (child == nullptr)
    return child;

  SharedNodeRef copy;
  if (node->rid() == rid_)
    copy = node;
  else {
    copy = Node::Copy(node, db_, rid_);
    fresh_nodes_.push_back(copy);
  }

  if (less)
    copy->left.set_ref(child);
  else
    copy->right.set_ref(child);

  return copy;
}

void PersistentTree::Copy(const zlog::Slice& prefixed_key)
{
  TraceApplier ta(this);

  auto base_root = root_ == nullptr ? src_root_.ref(trace_) : root_;
  auto root = copy_recursive(prefixed_key, base_root);
  if (root) {
    // an existing path is replaced, so no rebalance necessary.
    root_ = root;
  }
}

// TODO
//  - the Copy interface above is a fine basis for implementing a more efficient
//  update mechanism. Below it's handled inefficiently by deleting and
//  re-inserting. Boo.
void PersistentTree::Put(const zlog::Slice& prefixed_key,
    const zlog::Slice& value)
{
  TraceApplier ta(this);

  /*
   * build copy of path to new node
   */
  std::deque<SharedNodeRef> path;

  //src_root_.Print();
  auto base_root = root_ == nullptr ? src_root_.ref(trace_) : root_;
  auto root = insert_recursive(path, prefixed_key, value, base_root);
  if (root == nullptr) {
    /*
     * this is the update case that is transformed into delete + put. an
     * optimization would be to 1) use the path constructed here to skip that
     * step in delete or 2) update the algorithm to handle this case
     * explicitly.
     */
    Delete(prefixed_key); // first remove the key
    path.clear(); // new path will be built
    assert(root_ != nullptr); // delete set the root
    root = insert_recursive(path, prefixed_key, value, root_);
    assert(root != nullptr); // a new root was added
  }

  path.push_back(Node::Nil());
  assert(path.size() >= 2);

  /*
   * balance the tree
   */
  auto nn = pop_front(path);
  auto parent = pop_front(path);

  while (parent->red()) {
    assert(!path.empty());
    auto grand_parent = path.front();
    if (grand_parent->left.ref(trace_) == parent)
      insert_balance(parent, nn, path, left, right, root);
    else
      insert_balance(parent, nn, path, right, left, root);
  }

  root->set_red(false);

  assert(root != nullptr);
  root_ = root;
}

int PersistentTree::Get(const zlog::Slice& key, std::string* val)
{
  TraceApplier ta(this);

  auto cur = root_ == nullptr ? src_root_.ref(trace_) : root_;
  while (cur != Node::Nil()) {
    int cmp = key.compare(zlog::Slice(cur->key().data(),
          cur->key().size()));
    if (cmp == 0) {
      val->assign(cur->val().data(), cur->val().size());
      return 0;
    }
    cur = cmp < 0 ? cur->left.ref(trace_) :
      cur->right.ref(trace_);
  }
  return -ENOENT;
}

void PersistentTree::Delete(const zlog::Slice& key)
{
  TraceApplier ta(this);

  std::deque<SharedNodeRef> path;

  auto base_root = root_ == nullptr ? src_root_.ref(trace_) : root_;
  auto root = delete_recursive(path, key, base_root);
  if (root == nullptr) {
    return;
  }

  //roots.push_back(node);
  path.push_back(Node::Nil());
  assert(path.size() >= 2);

  /*
   * remove and balance
   */
  auto removed = path.front();
  assert(removed != nullptr);
  assert(removed->key() == key);

  auto transplanted = removed->right.ref(trace_);
  assert(transplanted != nullptr);

  if (removed->left.ref(trace_) == Node::Nil()) {
    path.pop_front();
    transplant(path.front(), removed, transplanted, root);
    assert(transplanted != nullptr);
  } else if (removed->right.ref(trace_) == Node::Nil()) {
    path.pop_front();
    assert(removed->left.ref(trace_) != nullptr);
    transplanted = removed->left.ref(trace_);
    transplant(path.front(), removed, transplanted, root);
    assert(transplanted != nullptr);
  } else {
    assert(transplanted != nullptr);
    auto temp = removed;
    if (removed->right.ref(trace_)->rid() != rid_) {
      auto n = Node::Copy(removed->right.ref(trace_), db_, rid_);
      fresh_nodes_.push_back(n);
      removed->right.set_ref(n);
    }
    removed = build_min_path(removed->right.ref(trace_), path);
    transplanted = removed->right.ref(trace_);
    assert(transplanted != nullptr);

    //temp->key = std::move(removed->key);
    //temp->val = std::move(removed->val);
    temp->steal_payload(removed);

    transplant(path.front(), removed, transplanted, root);
    assert(transplanted != nullptr);
  }

  if (!removed->red())
    balance_delete(transplanted, path, root);

  assert(root != nullptr);
  root_ = root;
}

}
