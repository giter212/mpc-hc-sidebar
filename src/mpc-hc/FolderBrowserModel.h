#pragma once
#include <Windows.h>
#include <Shlwapi.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct FolderEntry {
    std::wstring path, name, error;
    bool directory = false, expanded = false, loaded = false, loading = false;
    uint64_t created = 0, modified = 0, size = 0;
    std::vector<std::shared_ptr<FolderEntry>> children;
};
struct FolderViewRow { std::shared_ptr<FolderEntry> entry; int depth = 0; };
enum class FolderSort { Name, Created, Modified, Size };

inline uint64_t FolderFileTime(FILETIME time) {
    return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
inline std::wstring FolderJoin(const std::wstring& folder, const std::wstring& name) {
    return folder + (folder.empty() || folder.back() != L'\\' ? L"\\" : L"") + name;
}
inline std::vector<std::shared_ptr<FolderEntry>> ReadFolderEntries(
    const std::wstring& folder, const std::set<std::wstring>& extensions,
    const std::atomic<bool>& cancelled, std::wstring& error)
{
    std::vector<std::shared_ptr<FolderEntry>> entries;
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW(FolderJoin(folder, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) error = L"无法读取文件夹，请检查位置和访问权限。";
        return entries;
    }
    do {
        if (cancelled) break;
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        // Directory links/junctions can lead back to an ancestor or outside the selected tree.
        if (directory && (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (!directory) {
            std::wstring ext = PathFindExtensionW(data.cFileName);
            if (!ext.empty()) CharLowerBuffW(&ext[0], DWORD(ext.size()));
            if (!extensions.count(ext)) continue;
        }
        auto entry = std::make_shared<FolderEntry>();
        entry->name = data.cFileName;
        entry->path = FolderJoin(folder, entry->name);
        entry->directory = directory;
        entry->created = FolderFileTime(data.ftCreationTime);
        entry->modified = FolderFileTime(data.ftLastWriteTime);
        entry->size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        entries.push_back(std::move(entry));
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return entries;
}
inline void SortFolderEntries(FolderEntry& root, FolderSort sort, bool descending) {
    std::sort(root.children.begin(), root.children.end(), [=](const auto& a, const auto& b) {
        // Video files always precede child folders, regardless of sort direction.
        if (a->directory != b->directory) return !a->directory;
        int order = 0;
        if (sort != FolderSort::Name) {
            uint64_t av = sort == FolderSort::Created ? a->created : sort == FolderSort::Modified ? a->modified : a->size;
            uint64_t bv = sort == FolderSort::Created ? b->created : sort == FolderSort::Modified ? b->modified : b->size;
            order = av < bv ? -1 : av > bv ? 1 : 0;
        }
        if (!order) order = StrCmpLogicalW(a->name.c_str(), b->name.c_str());
        if (!order) order = a->path.compare(b->path);
        return descending ? order > 0 : order < 0;
    });
    for (auto& child : root.children) if (child->directory && child->loaded) SortFolderEntries(*child, sort, descending);
}
inline void FlattenFolderEntries(const FolderEntry& root, std::vector<FolderViewRow>& rows, int depth = 0) {
    for (const auto& entry : root.children) {
        rows.push_back({entry, depth});
        if (entry->directory && entry->expanded) FlattenFolderEntries(*entry, rows, depth + 1);
    }
}

// Pixel coordinates are shared by painting, keyboard scrolling and scrollbar tracking.
struct FolderRowLayout {
    std::vector<int> tops{0};
    void Reset(const std::vector<FolderViewRow>& rows, int videoHeight, int folderHeight) {
        tops.assign(1, 0);
        for (const auto& row : rows) tops.push_back(tops.back() + (row.entry->directory ? folderHeight : videoHeight));
    }
    int Total() const { return tops.back(); }
    int Clamp(int offset, int viewport) const { return std::clamp(offset, 0, std::max(0, Total() - viewport)); }
    int IndexAt(int offset) const {
        if (tops.size() < 2) return -1;
        return std::clamp(int(std::upper_bound(tops.begin(), tops.end(), offset) - tops.begin()) - 1, 0, int(tops.size()) - 2);
    }
};
