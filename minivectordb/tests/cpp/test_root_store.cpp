#include "root_store.hpp"
#include "table_file.hpp"
#include "test_storage_support.hpp"
#include <fstream>
#include <vector>
#include <string>
#ifdef MINIVECTORDB_TESTING
void recovery_reclamation_test(bool fail_sync) {
  TestDir d;
  RootStore store(d.path);
  auto s = schema();
  auto make = [&] {
    auto ref = store.create_object();
    TableFileWriter writer(store.object_path(ref), s);
    writer.finish();
    Catalog root;
    root.add(s, ref);
    return root;
  };
  auto previous = make();
  store.publish(previous);
  std::string previous_catalog;
  { std::ifstream current(d.path / "CURRENT"); std::getline(current, previous_catalog); }
  auto replacement = make();
  store.set_test_hook([](const std::string& event) {
    if (event == "current_replaced") throw DbError("storage_error", "injected");
  });
  rejects([&] { store.publish(replacement); });
  RootStore reopened(d.path);
  bool synced = false;
  bool removed = false;
  reopened.set_test_hook([&](const std::string& event) {
    if (fail_sync && event == "before_recovery_dir_sync")
      throw DbError("storage_error", "injected recovery sync failure");
    if (event == "recovery_dir_synced") synced = true;
    if (event == "before_garbage_remove") {
      check(synced);
      removed = true;
    }
  });
  if (fail_sync) {
    rejects([&] { reopened.recover(); });
    check(!synced && !removed);
    check(std::filesystem::exists(d.path / previous_catalog));
    check(std::filesystem::exists(store.object_path(previous.find("docs").objects.at(0))));
    check(std::filesystem::exists(store.object_path(replacement.find("docs").objects.at(0))));
    // A later successful startup can finish recovery and safely reclaim A.
    reopened.set_test_hook({});
  }
  check(reopened.recover().find("docs").objects.at(0) == replacement.find("docs").objects.at(0));
  if (!fail_sync) check(synced && removed);
  check(!std::filesystem::exists(d.path / previous_catalog));
  check(!std::filesystem::exists(store.object_path(previous.find("docs").objects.at(0))));
  check(std::filesystem::exists(store.object_path(replacement.find("docs").objects.at(0))));
}
void publication_tests() {
  TestDir d;
  RootStore store(d.path);
  auto s = schema();
  auto make = [&](TableSchema shape) {
    auto ref = store.create_object();
    TableFileWriter writer(store.object_path(ref), shape);
    writer.append({std::string("中"), std::int64_t(7), std::vector<float>{.1f, -2.f}});
    writer.finish();
    return ref;
  };
  Catalog committed;
  committed.add(s, make(s));
  auto second = s; second.name = "second";
  committed.add(second, make(second));
  std::vector<std::string> events;
  store.set_test_hook([&](const std::string& event) { events.push_back(event); });
  store.publish(committed);
  check(events == std::vector<std::string>{"objects_synced", "objects_dir_synced",
        "catalog_synced", "catalogs_dir_synced", "current_synced",
        "before_current_replace", "current_replaced", "data_dir_synced"});
  store.set_test_hook({});
  check(store.load_root().tables().size() == 2);
  Catalog transaction = committed;
  transaction.replace_object("docs", make(s));
  Catalog candidate = transaction;
  candidate.replace_object("second", make(second));
  auto orphan = make(s);
  auto unused_catalog = store.write_catalog(candidate);
  store.collect_garbage({transaction, candidate});
  check(!std::filesystem::exists(d.path / orphan));
  check(!std::filesystem::exists(d.path / unused_catalog));
  for (const auto* root : {&committed, &transaction, &candidate})
    for (const auto& [name, entry] : root->tables())
      for (const auto& object : entry.objects)
        check(std::filesystem::exists(store.object_path(object)));
  store.collect_garbage();
  check(!std::filesystem::exists(store.object_path(transaction.find("docs").objects.at(0))));
  check(std::distance(std::filesystem::directory_iterator(d.path/"objects"),
                      std::filesystem::directory_iterator{}) == 2);
  Catalog replacement = committed;
  replacement.replace_object("docs", make(s));
  // Each pre-replace failure leaves exactly the old multi-table root usable.
  for (const auto& stage : {"objects_synced", "objects_dir_synced", "catalog_synced",
                            "catalogs_dir_synced", "current_synced", "before_current_replace"}) {
    store.set_test_hook([&](const std::string& event) {
      if (event == stage) throw DbError("storage_error", "injected");
    });
    rejects([&] { store.publish(replacement); });
    check(store.load_root().find("docs").objects.at(0) == committed.find("docs").objects.at(0));
  }
  // A real failed rename, not a thrown checkpoint exception, poisons the
  // publishing instance and forbids both another publish and reclamation.
  {
    RootStore failing(d.path);
    failing.set_test_hook([&](const std::string& event) {
      if (event == "before_current_replace") {
        std::filesystem::rename(d.path / "CURRENT", d.path / "saved-current");
        std::filesystem::create_directory(d.path / "CURRENT");
      }
    });
    bool rename_fatal = false;
    try { failing.publish(replacement); }
    catch (const DbError& error) { rename_fatal = error.code == "fatal_storage_error"; }
    check(rename_fatal);
    rejects([&] { failing.publish(committed); });
    rejects([&] { failing.collect_garbage(); });
    std::filesystem::remove(d.path / "CURRENT");
    std::filesystem::rename(d.path / "saved-current", d.path / "CURRENT");
    check(RootStore(d.path).load_root().find("docs").objects.at(0) == committed.find("docs").objects.at(0));
  }
  // Once CURRENT may have changed, continuing to serve any root is forbidden.
  store.set_test_hook([](const std::string& event) {
    if (event == "current_replaced") throw DbError("storage_error", "injected");
  });
  bool fatal = false;
  try { store.publish(replacement); }
  catch (const DbError& error) { fatal = error.code == "fatal_storage_error"; }
  check(fatal);
  rejects([&] { store.publish(committed); });
  RootStore reopened(d.path);
  check(reopened.load_root().tables().size() == 2);
  check(reopened.load_root().find("docs").objects.at(0) == replacement.find("docs").objects.at(0));
  check(reopened.load_root().find("second").objects.at(0) == committed.find("second").objects.at(0));
  // Recovery validates all references before any orphan is reclaimed.
  auto evidence = reopened.create_object();
  std::filesystem::remove(reopened.object_path(replacement.find("docs").objects.at(0)));
  rejects([&] { reopened.recover(); });
  check(std::filesystem::exists(reopened.object_path(evidence)));
}
#endif
int main(int argc, char** argv) {
#ifdef MINIVECTORDB_TESTING
  if (argc > 1) {
    if (std::string(argv[1]) == "recovery-order") recovery_reclamation_test(false);
    else if (std::string(argv[1]) == "recovery-sync-failure") recovery_reclamation_test(true);
    else {
      publication_tests();
      recovery_reclamation_test(false);
      recovery_reclamation_test(true);
    }
    return 0;
  }
#else
  (void)argc;
  (void)argv;
#endif
  TestDir d;
  RootStore store(d.path);
  check(store.load_root().tables().empty());
  auto ref = store.create_object();
  auto other = store.create_object();
  check(ref != other);
  auto s = schema();
  {
    TableFileWriter w(store.object_path(ref), s);
    w.finish();
  }
  Catalog c;
  c.add(s, ref);
  check(c.find("docs").objects == std::vector<std::string>{ref});
  rejects([&] { c.add(s, other); });
  auto root = store.write_catalog(c);
  auto loaded = store.read_catalog(root);
  check(loaded.find("docs").schema.primary_key_index == 1);
  {
    std::ofstream f(d.path / "CURRENT");
    f << root << '\n';
  }
  check(store.load_root().find("docs").objects == std::vector<std::string>{ref});
  for (auto name : {"../escape", "/tmp/escape", "objects/../x",
                    "objects/sub/x.tbl", "objects/x.tbl/", "objects/x.bin"})
    rejects([&] { store.object_path(name); });
  auto sql = s;
  sql.name = "../../sql-name";
  Catalog sqlcat;
  sqlcat.add(sql, other);
  check(sqlcat.find(sql.name).objects == std::vector<std::string>{other});
  std::filesystem::create_symlink(d.path / "CURRENT",
                                  d.path / "objects" / "link.tbl");
  rejects([&] { store.object_path("objects/link.tbl"); });
  rejects([&] { store.read_catalog("../CURRENT"); });
  std::filesystem::resize_file(d.path / root,
                               std::filesystem::file_size(d.path / root) - 1);
  rejects([&] { store.load_root(); });
  {
    std::ofstream f(d.path / "CURRENT");
    f << "catalogs/missing.bin\n";
  }
  rejects([&] { store.load_root(); });
  Catalog bad;
  rejects([&] { bad.add(s, "../escape"); });
}
