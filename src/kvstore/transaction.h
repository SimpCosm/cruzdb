#ifndef ZLOG_KVSTORE_TRANSACTION_H
#define ZLOG_KVSTORE_TRANSACTION_H
#include <deque>
#include "node.h"
#include "kvstore.pb.h"

class DB;

/*
 * would be nice to have some mechanism here for really enforcing the idea
 * that this transaction is isolated.
 */
class Transaction {
 public:
  Transaction(DB *db, NodeRef root, uint64_t rid) :
    db_(db), root_(root), rid_(rid)
  {}

  void Put(std::string val);

  void Commit();

 private:
  DB *db_;
  NodeRef root_;
  uint64_t rid_;
  kvstore_proto::Intention intention_;

  static inline NodePtr& left(NodeRef n) { return n->left; };
  static inline NodePtr& right(NodeRef n) { return n->right; };

  static inline NodeRef pop_front(std::deque<NodeRef>& d) {
    auto front = d.front();
    d.pop_front();
    return front;
  }

  NodeRef insert_recursive(std::deque<NodeRef>& path,
      std::string elem, NodeRef& node, uint64_t rid);

  template<typename ChildA, typename ChildB>
  void insert_balance(NodeRef& parent, NodeRef& nn,
      std::deque<NodeRef>& path, ChildA, ChildB, NodeRef& root,
      uint64_t rid);

  template <typename ChildA, typename ChildB >
  NodeRef rotate(NodeRef parent, NodeRef child,
      ChildA child_a, ChildB child_b, NodeRef& root,
      uint64_t rid);

  void serialize_node_ptr(kvstore_proto::NodePtr *dst,
      NodePtr& src, uint64_t rid, const std::string& dir);

  void serialize_node(kvstore_proto::Node *n, NodeRef node,
      uint64_t rid, int field_index);

  void serialize_intention_recursive(kvstore_proto::Intention& i,
      uint64_t rid, NodeRef node, int& field_index);

  void serialize_intention(kvstore_proto::Intention& i, NodeRef node);

  void set_intention_self_csn_recursive(uint64_t rid, NodeRef node,
      uint64_t pos);

  void set_intention_self_csn(NodeRef root, uint64_t pos);
};

#endif
