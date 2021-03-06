#include <sstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <cstdlib>
#include <time.h>
#include <sys/time.h>
#include "cruzdb/db.h"
#include "db/cruzdb.pb.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

using namespace rapidjson;

int main(int argc, char **argv)
{
  bool dump_val = false;
  if (argc >= 3 && atoi(argv[2]))
    dump_val = true;

  zlog::Log *log;
  int ret = zlog::Log::Open("lmdb", "log", {{"path", argv[1]}}, "", "", &log);
  assert(ret == 0);

  uint64_t tail;
  ret = log->CheckTail(&tail);
  assert(ret == 0);

  std::cerr << "tail: " << tail << std::endl;

  uint64_t pos = 0;
  do {
    std::string data;
    ret = log->Read(pos, &data);
    if (ret == 0) {
      cruzdb_proto::AfterImage i;
      assert(i.ParseFromString(data));
      assert(i.IsInitialized());

      StringBuffer s;
      Writer<StringBuffer> writer(s);

      writer.StartObject();
      writer.Key("pos");
      writer.Uint(pos);
      writer.Key("bytes");
      writer.Uint(data.size());
#if 0 // FIXME: not valid anymore
      writer.Key("snapshot");
      writer.Uint(i.snapshot());
#endif
      writer.Key("tree");
      writer.StartArray();

      for (int j = 0; j < i.tree_size(); j++) {
        const auto& node = i.tree(j);

        writer.StartObject();

        writer.Key("red");
        writer.Bool(node.red());

        writer.Key("key");
        writer.String(node.key().c_str());

        writer.Key("val");
        if (dump_val)
          writer.String(node.val().c_str());
        else
          writer.String("");

        writer.Key("left");
        writer.StartObject();
        writer.Key("nil");
        writer.Bool(node.left().nil());
        writer.Key("self");
        writer.Bool(node.left().self());
        // TODO: this format of the log has changed since this was built and the
        // export now needs updated, as well as the parser...
#if 0
        writer.Key("csn");
        writer.Uint(node.left().csn());
#endif
        writer.Key("off");
        writer.Uint(node.left().off());
        writer.EndObject();

        writer.Key("right");
        writer.StartObject();
        writer.Key("nil");
        writer.Bool(node.right().nil());
        writer.Key("self");
        writer.Bool(node.right().self());
        // TODO: see above
#if 0
        writer.Key("csn");
        writer.Uint(node.right().csn());
#endif
        writer.Key("off");
        writer.Uint(node.right().off());
        writer.EndObject();

        writer.EndObject();
      }

      writer.EndArray();
      writer.EndObject();

      std::cout << s.GetString() << std::endl;

    } else {
      std::cerr << "pos " << pos << " err " << ret << std::endl;
    }
  } while (++pos <= tail);

  return 0;
}
