#include "../src/mpc-hc/FolderBrowserModel.h"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

static std::shared_ptr<FolderEntry> Make(const wchar_t* name, bool dir = false, uint64_t time = 0, uint64_t size = 0) {
    auto e = std::make_shared<FolderEntry>();
    e->name = e->path = name; e->directory = dir; e->created = time; e->modified = time + 1; e->size = size;
    return e;
}
int wmain(int argc, wchar_t** argv) {
    assert(argc == 2);
    FolderEntry root;
    auto folder = Make(L"子文件夹", true);
    auto a = Make(L"video2.mp4", false, 1, 0x100000000ULL);
    auto b = Make(L"video10.mp4", false, 0x100000000ULL, 1);
    root.children = {folder, b, a};
    auto nested = Make(L"深层", true); nested->children = {Make(L"child.mp4")}; nested->loaded = true;
    folder->children = {nested, Make(L"inner.mp4")}; folder->loaded = true;
    SortFolderEntries(root, FolderSort::Name, false);
    assert(root.children[0] == a && root.children[1] == b && root.children[2] == folder);
    std::vector<FolderViewRow> rows; FlattenFolderEntries(root, rows);
    assert(rows.size() == 3 && !folder->expanded);
    folder->expanded = true; rows.clear(); FlattenFolderEntries(root, rows);
    assert(rows.size() == 5 && rows[3].entry->name == L"inner.mp4" && rows[3].depth == 1);
    nested->expanded = true; rows.clear(); FlattenFolderEntries(root, rows);
    assert(rows.size() == 6 && rows.back().depth == 2);
    folder->expanded = false; rows.clear(); FlattenFolderEntries(root, rows); assert(rows.size() == 3);
    SortFolderEntries(root, FolderSort::Created, true); assert(root.children[0] == b && root.children.back() == folder);
    SortFolderEntries(root, FolderSort::Modified, false); assert(root.children[0] == a);
    SortFolderEntries(root, FolderSort::Size, true); assert(root.children[0] == a);
    SortFolderEntries(root, FolderSort::Name, true); assert(root.children[0] == b && root.children.back() == folder);
    FolderRowLayout layout; layout.Reset(rows, 104, 36);
    assert(layout.Total() == 244 && layout.IndexAt(103) == 0 && layout.IndexAt(104) == 1);
    assert(layout.IndexAt(208) == 2 && layout.Clamp(10000, 100) == 144);
    assert(layout.Clamp(-1, 100) == 0 && layout.Clamp(10, 1000) == 0);
    layout.Reset({}, 104, 36); assert(layout.IndexAt(0) == -1 && layout.Clamp(100, 400) == 0);

    // Enumerate a real nested directory, including Unicode, ampersands, empty and missing folders.
    namespace fs = std::filesystem;
    fs::path fixture = argv[1];
    assert(!fs::exists(fixture)); fs::create_directories(fixture / L"子目录" / L"深层");
    fs::create_directory(fixture / L"空目录");
    std::ofstream(fixture / L"video2.MP4") << "x";
    std::ofstream(fixture / L"中文 & video10.mp4") << "abc";
    std::ofstream(fixture / L"ignore.txt") << "not a video";
    std::ofstream(fixture / L"子目录" / L"nested.mp4") << "xx";
    std::atomic<bool> cancelled{false}; std::wstring error;
    auto entries = ReadFolderEntries(fixture.wstring(), {L".mp4"}, cancelled, error);
    assert(error.empty() && entries.size() == 4);
    size_t dirs = 0; for (auto& e : entries) if (e->directory) { ++dirs; assert(!e->expanded && e->children.empty()); }
    assert(dirs == 2);
    auto empty = ReadFolderEntries((fixture / L"空目录").wstring(), {L".mp4"}, cancelled, error);
    assert(empty.empty() && error.empty());
    auto missing = ReadFolderEntries((fixture / L"不存在").wstring(), {L".mp4"}, cancelled, error);
    assert(missing.empty() && !error.empty());
    error.clear(); cancelled = true;
    assert(ReadFolderEntries(fixture.wstring(), {L".mp4"}, cancelled, error).empty());
    assert(FolderJoin(L"E:\\", L"test") == L"E:\\test");

    auto start = std::chrono::steady_clock::now();
    rows.assign(100000, {a, 0}); layout.Reset(rows, 104, 36);
    assert(layout.Total() == 10400000);
    for (int i = 0; i < 100000; ++i) {
        int offset = i % 2 ? layout.Total() - i : i * 103;
        int clamped = layout.Clamp(offset, 480);
        int index = layout.IndexAt(clamped);
        assert(layout.tops[index] <= clamped && clamped < layout.tops[index + 1]);
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    std::cout << "Folder browser tests passed; 100000-row geometry and alternating scroll checks: " << ms << " ms\n";
}
