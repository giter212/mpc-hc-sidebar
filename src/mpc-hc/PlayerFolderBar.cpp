#include "stdafx.h"
#include "PlayerFolderBar.h"
#include "MainFrm.h"
#include "mplayerc.h"
#include "CMPCTheme.h"
#include <afxdlgs.h>
#include <atlimage.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace {
constexpr UINT ChooseID = 20001, RefreshID = 20002, CollapseID = 20003, ListID = 20004;
constexpr UINT RecentID = 20005, SortID = 20006, ScanTimer = 71;
constexpr UINT FirstVideoButton = 20100, LastVideoButton = 20355;
const wchar_t* Section = L"VideoFolder";
}

struct FolderThumbnail {
    HBITMAP bitmap = nullptr;
    uint64_t lastUsed = 0;
    ~FolderThumbnail() { if (bitmap) DeleteObject(bitmap); }
};
struct FolderDirectoryResult {
    std::shared_ptr<FolderEntry> parent;
    std::vector<std::shared_ptr<FolderEntry>> children;
    std::wstring error;
};
struct FolderBrowserWorker {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::shared_ptr<FolderEntry>> directories;
    std::deque<std::wstring> thumbnails;
    std::vector<FolderDirectoryResult> directoryResults;
    std::vector<std::pair<std::wstring, std::shared_ptr<FolderThumbnail>>> imageResults;
    std::set<std::wstring> extensions, requestedImages;
    std::wstring helper, cache, inFlight;
    void Cancel() { cancelled = true; wake.notify_one(); }
    bool WantsImage(const std::wstring& file) {
        std::lock_guard<std::mutex> lock(mutex);
        return !cancelled && directories.empty() && requestedImages.count(file) != 0;
    }
    void RequestDirectory(const std::shared_ptr<FolderEntry>& entry) {
        entry->loading = true;
        { std::lock_guard<std::mutex> lock(mutex); directories.push_back(entry); }
        wake.notify_one();
    }
};

static bool ExtractFrame(const std::wstring& file, const std::wstring& output, int second,
                         const std::shared_ptr<FolderBrowserWorker>& worker)
{
    CString command;
    command.Format(L"\"%s\" -hide_banner -loglevel error -nostdin -y -threads 1 -ss %d -i \"%s\" -map 0:v:0 -frames:v 1 -an -sn -vf \"scale=240:135:force_original_aspect_ratio=decrease,pad=240:135:(ow-iw)/2:(oh-ih)/2\" \"%s\"",
                   worker->helper.c_str(), second, file.c_str(), output.c_str());
    STARTUPINFO startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    BOOL started = CreateProcess(worker->helper.c_str(), command.GetBuffer(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup, &process);
    command.ReleaseBuffer();
    if (!started) return false;
    CloseHandle(process.hThread);
    auto start = GetTickCount64();
    while (WaitForSingleObject(process.hProcess, 100) == WAIT_TIMEOUT) {
        if (!worker->WantsImage(file) || GetTickCount64() - start > 12000) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 2000);
            CloseHandle(process.hProcess);
            DeleteFile(output.c_str());
            return false;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return code == 0 && GetFileAttributes(output.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::shared_ptr<FolderThumbnail> LoadThumbnail(const std::wstring& file,
    const std::shared_ptr<FolderBrowserWorker>& worker)
{
    WIN32_FILE_ATTRIBUTE_DATA attr = {};
    if (!GetFileAttributesEx(file.c_str(), GetFileExInfoStandard, &attr)) return {};
    CString identity;
    identity.Format(L"%s|%lu|%lu|%lu|%lu", file.c_str(), attr.nFileSizeHigh,
        attr.nFileSizeLow, attr.ftLastWriteTime.dwHighDateTime, attr.ftLastWriteTime.dwLowDateTime);
    uint64_t hash = 14695981039346656037ULL;
    for (int c = 0; c < identity.GetLength(); ++c) { hash ^= identity[c]; hash *= 1099511628211ULL; }
    CString imagePath;
    imagePath.Format(L"%s\\%016llx.png", worker->cache.c_str(), hash);
    if (GetFileAttributes(imagePath) == INVALID_FILE_ATTRIBUTES) {
        CString temporary;
        temporary.Format(L"%s.%lu.%llu.png", imagePath.GetString(), GetCurrentThreadId(), GetTickCount64());
        bool ok = ExtractFrame(file, temporary.GetString(), 3, worker);
        if (!ok && worker->WantsImage(file)) ok = ExtractFrame(file, temporary.GetString(), 0, worker);
        if (ok && !worker->cancelled) MoveFileEx(temporary, imagePath, MOVEFILE_REPLACE_EXISTING);
        DeleteFile(temporary);
    }
    if (worker->cancelled) return {};
    CImage loaded;
    if (FAILED(loaded.Load(imagePath))) return {};
    auto image = std::make_shared<FolderThumbnail>();
    image->bitmap = loaded.Detach();
    return image;
}

static void RunFolderWorker(std::shared_ptr<FolderBrowserWorker> worker)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SHCreateDirectoryEx(nullptr, worker->cache.c_str(), nullptr);
    while (!worker->cancelled) {
        std::shared_ptr<FolderEntry> directory;
        std::wstring file;
        {
            std::unique_lock<std::mutex> lock(worker->mutex);
            worker->wake.wait(lock, [&] { return worker->cancelled || !worker->directories.empty() || !worker->thumbnails.empty(); });
            if (worker->cancelled) break;
            if (!worker->directories.empty()) {
                directory = worker->directories.front(); worker->directories.pop_front();
            } else {
                file = worker->thumbnails.front(); worker->thumbnails.pop_front(); worker->inFlight = file;
            }
        }
        if (directory) {
            FolderDirectoryResult result;
            result.parent = directory;
            result.children = ReadFolderEntries(directory->path, worker->extensions, worker->cancelled, result.error);
            std::lock_guard<std::mutex> lock(worker->mutex);
            if (!worker->cancelled) worker->directoryResults.push_back(std::move(result));
        } else {
            auto image = LoadThumbnail(file, worker);
            std::lock_guard<std::mutex> lock(worker->mutex);
            if (!worker->cancelled && worker->directories.empty() && worker->requestedImages.count(file)) worker->imageResults.emplace_back(file, std::move(image));
            worker->inFlight.clear();
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

BEGIN_MESSAGE_MAP(CFolderVideoList, CWnd)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_WM_SIZE()
    ON_WM_VSCROLL()
    ON_WM_MOUSEWHEEL()
    ON_WM_DRAWITEM()
    ON_CONTROL_RANGE(BN_CLICKED, FirstVideoButton, LastVideoButton, OnRowClicked)
END_MESSAGE_MAP()

BOOL CFolderVideoList::Create(const CRect& rect, CWnd* parent, UINT id)
{
    if (!CWnd::CreateEx(0, AfxRegisterWndClass(0, LoadCursor(nullptr, IDC_ARROW)), L"视频列表",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN, rect, parent, id)) return FALSE;
    if (!m_scrollbar.Create(WS_CHILD | SBS_VERT, CRect(0, 0, 1, 1), this, 1)) return FALSE;
    m_scrollbar.setScrollWindow(this);
    return TRUE;
}

void CFolderVideoList::SetRows(std::vector<FolderViewRow> rows, bool preservePosition)
{
    std::shared_ptr<FolderEntry> anchor, selected;
    int first = m_layout.IndexAt(m_offset), intra = 0;
    if (preservePosition && first >= 0) { anchor = m_rows[first].entry; intra = m_offset - m_layout.tops[first]; }
    if (preservePosition && m_selected >= 0 && m_selected < int(m_rows.size())) selected = m_rows[m_selected].entry;
    m_rows = std::move(rows);
    m_layout.Reset(m_rows, m_videoHeight, m_folderHeight);
    m_selected = -1;
    if (!preservePosition) m_offset = m_wheelRemainder = 0;
    for (int i = 0; i < int(m_rows.size()); ++i) {
        if (m_rows[i].entry == anchor) m_offset = m_layout.tops[i] + intra;
        if (m_rows[i].entry == selected) m_selected = i;
    }
    LayoutRows();
}

std::vector<int> CFolderVideoList::VisibleIndices() const
{
    std::vector<int> visible;
    CRect client; GetClientRect(client);
    int first = m_layout.IndexAt(m_offset);
    if (first < 0) return visible;
    for (int i = first; i < int(m_rows.size()) && m_layout.tops[i] < m_offset + client.Height(); ++i) visible.push_back(i);
    return visible;
}

void CFolderVideoList::LayoutRows()
{
    if (!m_scrollbar.GetSafeHwnd()) return;
    CRect client; GetClientRect(client);
    m_offset = m_layout.Clamp(m_offset, client.Height());
    bool needsScroll = m_layout.Total() > client.Height();
    int scrollWidth = needsScroll ? std::max(12, m_videoHeight * 14 / 104) : 0;
    int contentWidth = std::max(1, client.Width() - scrollWidth);
    CRect wanted(contentWidth, 0, contentWidth + scrollWidth, std::max(1, client.Height())), current;
    m_scrollbar.GetWindowRect(current); ScreenToClient(current);
    if (current != wanted) m_scrollbar.MoveWindow(wanted, FALSE);
    SCROLLINFO old = {sizeof(old), SIF_RANGE | SIF_PAGE | SIF_POS};
    m_scrollbar.GetScrollInfo(&old);
    SCROLLINFO info = {sizeof(info), SIF_POS};
    info.nMax = std::max(0, m_layout.Total() - 1);
    info.nPage = std::max(1, client.Height()); info.nPos = m_offset;
    if (old.nMax != info.nMax || old.nPage != info.nPage) info.fMask |= SIF_RANGE | SIF_PAGE;
    if (old.nPos != info.nPos || info.fMask != SIF_POS) m_scrollbar.SetScrollInfo(&info);
    if (!!(m_scrollbar.GetStyle() & WS_VISIBLE) != needsScroll) m_scrollbar.ShowWindow(needsScroll ? SW_SHOW : SW_HIDE);

    auto visible = VisibleIndices();
    size_t count = std::min(visible.size(), size_t(LastVideoButton - FirstVideoButton + 1));
    while (m_buttons.size() < count) {
        auto button = std::make_unique<CButton>();
        button->Create(L"", WS_CHILD | BS_OWNERDRAW, CRect(0, 0, 1, 1), this, FirstVideoButton + UINT(m_buttons.size()));
        m_buttons.push_back(std::move(button)); m_buttonIndices.push_back(-1);
    }

    for (size_t slot = 0; slot < m_buttons.size(); ++slot) {
        auto& button = *m_buttons[slot];
        if (slot < count) {
            int index = visible[slot];
            m_buttonIndices[slot] = index;
            CString previous; button.GetWindowText(previous);
            if (previous != m_rows[index].entry->name.c_str()) button.SetWindowText(m_rows[index].entry->name.c_str());
            int y = m_layout.tops[index] - m_offset, height = m_layout.tops[index + 1] - m_layout.tops[index];
            UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_SHOWWINDOW;
            button.SetWindowPos(nullptr, 0, y, contentWidth, height, flags);
        } else {
            m_buttonIndices[slot] = -1;
            if (button.GetStyle() & WS_VISIBLE) button.ShowWindow(SW_HIDE);
        }
    }

    RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void CFolderVideoList::ScrollTo(int offset)
{
    CRect client; GetClientRect(client);
    offset = m_layout.Clamp(offset, client.Height());
    if (offset == m_offset) return;
    m_offset = offset; LayoutRows();
}
void CFolderVideoList::OnPaint()
{
    CPaintDC dc(this); CRect client; GetClientRect(client);
    dc.FillSolidRect(client, AppIsThemeLoaded() ? CMPCTheme::ContentBGColor : GetSysColor(COLOR_WINDOW));
}
void CFolderVideoList::OnSize(UINT type, int cx, int cy) { CWnd::OnSize(type, cx, cy); LayoutRows(); }
void CFolderVideoList::OnVScroll(UINT code, UINT, CScrollBar*)
{
    CRect client; GetClientRect(client);
    int offset = m_offset;
    SCROLLINFO info = {sizeof(info), SIF_TRACKPOS};
    switch (code) {
        case SB_LINEUP: offset -= std::max(1, m_videoHeight / 4); break;
        case SB_LINEDOWN: offset += std::max(1, m_videoHeight / 4); break;
        case SB_PAGEUP: offset -= std::max(1, client.Height() - m_folderHeight); break;
        case SB_PAGEDOWN: offset += std::max(1, client.Height() - m_folderHeight); break;
        case SB_TOP: offset = 0; break;
        case SB_BOTTOM: offset = m_layout.Total(); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            m_scrollbar.GetScrollInfo(&info); offset = info.nTrackPos; break;
        default: return;
    }
    ScrollTo(offset);
}
BOOL CFolderVideoList::OnMouseWheel(UINT, short delta, CPoint)
{
    UINT lines = 3; SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    CRect client; GetClientRect(client);
    int step = lines == WHEEL_PAGESCROLL ? client.Height() : std::max(1, m_videoHeight / 6) * int(lines);
    m_wheelRemainder += delta * step;
    int pixels = m_wheelRemainder / WHEEL_DELTA; m_wheelRemainder %= WHEEL_DELTA;
    ScrollTo(m_offset - pixels); return TRUE;
}
void CFolderVideoList::SelectItem(int index)
{
    if (m_rows.empty()) return;
    m_selected = std::clamp(index, 0, int(m_rows.size()) - 1);
    CRect client; GetClientRect(client);
    if (m_layout.tops[m_selected] < m_offset) ScrollTo(m_layout.tops[m_selected]);
    else if (m_layout.tops[m_selected + 1] > m_offset + client.Height()) ScrollTo(m_layout.tops[m_selected + 1] - client.Height());
    for (size_t slot = 0; slot < m_buttons.size(); ++slot) {
        if (m_buttonIndices[slot] == m_selected) m_buttons[slot]->SetFocus();
        m_buttons[slot]->Invalidate(FALSE);
    }
}
void CFolderVideoList::ActivateSelection()
{
    if (m_selected < 0) return;
    NMITEMACTIVATE notification = {};
    notification.hdr = {m_hWnd, UINT_PTR(GetDlgCtrlID()), NM_CLICK}; notification.iItem = m_selected;
    GetParent()->SendMessage(WM_NOTIFY, GetDlgCtrlID(), LPARAM(&notification));
}
void CFolderVideoList::OnRowClicked(UINT id)
{
    size_t slot = id - FirstVideoButton;
    if (slot >= m_buttonIndices.size() || m_buttonIndices[slot] < 0) return;
    m_selected = m_buttonIndices[slot]; RedrawItems(0, int(m_rows.size())); ActivateSelection();
}
BOOL CFolderVideoList::PreTranslateMessage(MSG* message)
{
    if (message->message == WM_KEYDOWN) {
        int selected = m_selected < 0 ? std::max(0, m_layout.IndexAt(m_offset)) : m_selected;
        CRect client; GetClientRect(client);
        switch (message->wParam) {
            case VK_UP: SelectItem(selected - 1); return TRUE;
            case VK_DOWN: SelectItem(selected + 1); return TRUE;
            case VK_PRIOR: SelectItem(m_layout.IndexAt(std::max(0, m_layout.tops[selected] - client.Height()))); return TRUE;
            case VK_NEXT: SelectItem(m_layout.IndexAt(m_layout.tops[selected] + client.Height())); return TRUE;
            case VK_HOME: SelectItem(0); return TRUE;
            case VK_END: SelectItem(int(m_rows.size()) - 1); return TRUE;
            case VK_LEFT:
            case VK_RIGHT:
                if (m_selected >= 0 && m_rows[m_selected].entry->directory &&
                    m_rows[m_selected].entry->expanded != (message->wParam == VK_RIGHT)) ActivateSelection();
                return TRUE;
            case VK_RETURN:
            case VK_SPACE: ActivateSelection(); return TRUE;
        }
    }
    return CWnd::PreTranslateMessage(message);
}
void CFolderVideoList::RedrawItems(int first, int last)
{
    for (size_t slot = 0; slot < m_buttons.size(); ++slot)
        if (m_buttonIndices[slot] >= first && m_buttonIndices[slot] <= last) m_buttons[slot]->Invalidate(FALSE);
}
void CFolderVideoList::OnDrawItem(int, LPDRAWITEMSTRUCT item)
{
    size_t slot = item->CtlID - FirstVideoButton;
    if (slot >= m_buttonIndices.size() || m_buttonIndices[slot] < 0) return;
    DRAWITEMSTRUCT forwarded = *item;
    forwarded.itemID = m_buttonIndices[slot]; forwarded.itemState &= ~ODS_SELECTED;
    if (m_selected == int(forwarded.itemID)) forwarded.itemState |= ODS_SELECTED;
    GetParent()->SendMessage(WM_DRAWITEM, GetDlgCtrlID(), LPARAM(&forwarded));
}

CPlayerFolderBar::CPlayerFolderBar(CMainFrame* frame) : CMPCThemePlayerBar(frame) {}
CPlayerFolderBar::~CPlayerFolderBar() { StopWorker(); }
int CPlayerFolderBar::Scale(int value) const { return MulDiv(value, m_scale, 96); }
void CPlayerFolderBar::StopWorker() { if (m_worker) m_worker->Cancel(); m_worker.reset(); }

BEGIN_MESSAGE_MAP(CPlayerFolderBar, CMPCThemePlayerBar)
    ON_BN_CLICKED(ChooseID, OnChoose)
    ON_BN_CLICKED(RecentID, OnRecent)
    ON_BN_CLICKED(RefreshID, OnRefresh)
    ON_BN_CLICKED(CollapseID, OnCollapse)
    ON_CBN_SELCHANGE(SortID, OnSortChanged)
    ON_NOTIFY(NM_CLICK, ListID, OnClick)
    ON_WM_SIZE()
    ON_WM_TIMER()
    ON_WM_DESTROY()
    ON_WM_DRAWITEM()
    ON_WM_CTLCOLOR()
END_MESSAGE_MAP()

BOOL CPlayerFolderBar::Create(CWnd* parent, UINT dockID)
{
    if (!__super::Create(L"视频文件夹", parent, ID_VIEW_VIDEOFOLDER, dockID, L"Video Folder Bar")) return FALSE;
    CClientDC dc(this); m_scale = dc.GetDeviceCaps(LOGPIXELSX);
    m_font.CreateFont(-Scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    CRect r(0, 0, 10, 10);
    m_choose.Create(L"选择文件夹", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, r, this, ChooseID);
    m_recent.Create(L"打开上次文件夹", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, r, this, RecentID);
    m_refresh.Create(L"刷新", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, r, this, RefreshID);
    m_collapse.Create(L"折叠", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, r, this, CollapseID);
    m_sort.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, r, this, SortID);
    for (auto label : {L"名称 ↑", L"名称 ↓", L"创建时间 ↑", L"创建时间 ↓", L"修改时间 ↑", L"修改时间 ↓", L"大小 ↑", L"大小 ↓"}) m_sort.AddString(label);
    m_sort.SetCurSel(std::clamp(int(AfxGetApp()->GetProfileInt(Section, L"SortOrder", 0)), 0, 7));
    m_path.Create(L"", WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS, r, this);
    m_status.Create(L"选择文件夹以浏览视频", WS_CHILD | WS_VISIBLE | SS_LEFT, r, this);
    if (!m_list.Create(r, this, ListID)) return FALSE;
    m_list.SetRowHeights(Scale(104), Scale(36));
    for (CWnd* wnd : std::initializer_list<CWnd*>{&m_choose, &m_recent, &m_refresh, &m_collapse, &m_sort, &m_path, &m_status, &m_list}) wnd->SetFont(&m_font);
    m_sort.SetItemHeight(-1, Scale(26)); m_sort.SetItemHeight(0, Scale(28));
    m_recent.EnableWindow(FALSE); m_refresh.EnableWindow(FALSE);
    m_szMinVert = CSize(Scale(300), Scale(244)); m_szVert = CSize(Scale(340), Scale(460));
    m_szMinHorz = m_szMinFloat = m_szMinVert; m_szHorz = m_szFloat = m_szVert;
    return TRUE;
}
void CPlayerFolderBar::ReloadTranslatableResources() { SetWindowText(L"视频文件夹"); }
void CPlayerFolderBar::LoadState(CFrameWnd* parent)
{
    if (AfxGetApp()->GetProfileInt(Section, L"Visible", TRUE)) SetAutohidden(true);
    __super::LoadState(parent);
    m_lastFolder = AfxGetApp()->GetProfileString(Section, L"Folder");
    m_recent.EnableWindow(!m_lastFolder.IsEmpty());
    // Opening the app never enumerates the remembered folder.
}
void CPlayerFolderBar::SaveState()
{
    __super::SaveState();
    AfxGetApp()->WriteProfileInt(Section, L"Visible", IsWindowVisible() || IsAutohidden());
}
void CPlayerFolderBar::OnSize(UINT type, int cx, int cy)
{
    __super::OnSize(type, cx, cy);
    if (!m_list.GetSafeHwnd()) return;
    int pad = Scale(8), height = Scale(32);
    m_choose.MoveWindow(pad, pad, Scale(112), height);
    m_refresh.MoveWindow(pad + Scale(120), pad, Scale(56), height);
    m_collapse.MoveWindow(cx - Scale(64), pad, Scale(56), height);
    m_recent.MoveWindow(pad, Scale(48), Scale(144), height);
    m_sort.MoveWindow(Scale(160), Scale(48), std::max(Scale(120), cx - Scale(168)), Scale(320));
    m_path.MoveWindow(pad, Scale(88), std::max(1, cx - 2 * pad), Scale(24));
    m_list.MoveWindow(pad, Scale(120), std::max(1, cx - 2 * pad), std::max(1, cy - Scale(152)));
    m_status.MoveWindow(pad, std::max(Scale(120), cy - Scale(28)), std::max(1, cx - 2 * pad), Scale(24));
}
void CPlayerFolderBar::OnChoose()
{
    CFolderPickerDialog dialog(m_folder.IsEmpty() ? m_lastFolder : m_folder, OFN_PATHMUSTEXIST, this);
    if (dialog.DoModal() == IDOK) Browse(dialog.GetPathName());
}
void CPlayerFolderBar::OnRecent() { if (!m_lastFolder.IsEmpty()) Browse(m_lastFolder); }
void CPlayerFolderBar::OnRefresh() { if (!m_folder.IsEmpty()) Browse(m_folder); }
void CPlayerFolderBar::OnCollapse() { m_pMainFrame->PostMessage(WM_COMMAND, ID_VIEW_VIDEOFOLDER); }
void CPlayerFolderBar::OnSortChanged()
{
    AfxGetApp()->WriteProfileInt(Section, L"SortOrder", m_sort.GetCurSel());
    RebuildRows(false);
}
void CPlayerFolderBar::Browse(const CString& folder)
{
    // Copy first: the argument may alias m_lastFolder or m_folder.
    CString selected(folder);
    StopWorker(); m_images.clear(); m_failedImages.clear();
    m_folder = m_lastFolder = selected; m_path.SetWindowText(selected);
    m_recent.EnableWindow(TRUE); m_refresh.EnableWindow(TRUE);
    m_root = std::make_shared<FolderEntry>(); m_root->directory = true; m_root->path = selected.GetString();
    m_rows.clear(); m_list.SetRows({}, false); m_status.SetWindowText(L"正在读取文件夹…");
    AfxGetApp()->WriteProfileString(Section, L"Folder", selected);
    m_worker = std::make_shared<FolderBrowserWorker>();
    const auto& formats = AfxGetAppSettings().m_Formats;
    for (size_t i = 0; i < formats.GetCount(); ++i) {
        if (!formats[i].IsVideoOnly()) continue;
        CString values = formats[i].GetExtsWithPeriod(); int start = 0;
        CString value = values.Tokenize(L" ;", start);
        while (!value.IsEmpty()) { value.MakeLower(); m_worker->extensions.insert(value.GetString()); value = values.Tokenize(L" ;", start); }
    }
    wchar_t executable[MAX_PATH]; GetModuleFileName(nullptr, executable, MAX_PATH); PathRemoveFileSpec(executable);
    m_worker->helper = std::wstring(executable) + L"\\SidebarTools\\ffmpeg.exe";
    CString cache = AfxGetApp()->GetProfileString(Section, L"CachePath");
    if (cache.IsEmpty()) {
        CString settingsPath;
        if (!AfxGetMyApp()->GetAppSavePath(settingsPath)) settingsPath = executable;
        cache = FolderJoin(settingsPath.GetString(), L"FolderCache").c_str();
    }
    m_worker->cache = cache.GetString();
    m_worker->RequestDirectory(m_root);
    std::thread(RunFolderWorker, m_worker).detach();
    SetTimer(ScanTimer, 60, nullptr);
}
void CPlayerFolderBar::RebuildRows(bool preservePosition)
{
    if (!m_root) return;
    int sort = std::max(0, m_sort.GetCurSel());
    SortFolderEntries(*m_root, FolderSort(sort / 2), (sort % 2) != 0);
    m_rows.clear(); FlattenFolderEntries(*m_root, m_rows); m_list.SetRows(m_rows, preservePosition);

    CString status;
    if (!m_root->error.empty()) status = m_root->error.c_str();
    else if (m_rows.empty()) status = L"此文件夹中没有视频或子文件夹";
    else {
        size_t videos = 0, folders = 0;
        for (const auto& row : m_rows) row.entry->directory ? ++folders : ++videos;
        status.Format(L"%zu 个视频，%zu 个文件夹", videos, folders);
    }
    m_status.SetWindowText(status); RequestVisibleImages();
}
void CPlayerFolderBar::RequestVisibleImages()
{
    if (!m_worker) return;
    std::vector<std::wstring> wanted;
    for (int index : IsWindowVisible() ? m_list.VisibleIndices() : std::vector<int>{}) {
        auto& entry = *m_rows[index].entry;
        if (!entry.directory && !m_images.count(entry.path) && !m_failedImages.count(entry.path)) wanted.push_back(entry.path);
    }


    {
        std::lock_guard<std::mutex> lock(m_worker->mutex);
        m_worker->requestedImages = std::set<std::wstring>(wanted.begin(), wanted.end());
        m_worker->thumbnails.clear();
        for (const auto& path : wanted) if (path != m_worker->inFlight) m_worker->thumbnails.push_back(path);
    }
    m_worker->wake.notify_one();
}
void CPlayerFolderBar::OnTimer(UINT_PTR id)
{
    if (id != ScanTimer || !m_worker) { __super::OnTimer(id); return; }
    std::vector<FolderDirectoryResult> directories;
    std::vector<std::pair<std::wstring, std::shared_ptr<FolderThumbnail>>> images;
    {
        std::lock_guard<std::mutex> lock(m_worker->mutex);
        directories.swap(m_worker->directoryResults); images.swap(m_worker->imageResults);
    }
    for (auto& result : directories) {
        result.parent->children = std::move(result.children); result.parent->error = std::move(result.error);
        result.parent->loaded = true; result.parent->loading = false;
    }
    if (!directories.empty()) RebuildRows(true);
    for (auto& image : images) {
        if (image.second) { image.second->lastUsed = GetTickCount64(); m_images[image.first] = std::move(image.second); }
        else m_failedImages.insert(image.first);
    }
    while (m_images.size() > 128) {
        auto oldest = std::min_element(m_images.begin(), m_images.end(), [](const auto& a, const auto& b) { return a.second->lastUsed < b.second->lastUsed; });
        m_images.erase(oldest);
    }
    if (!images.empty()) { m_list.RedrawItems(0, int(m_rows.size())); }
    RequestVisibleImages();
}
void CPlayerFolderBar::OnDestroy()
{
    KillTimer(ScanTimer); StopWorker(); m_images.clear(); __super::OnDestroy();
}
void CPlayerFolderBar::PlaySelection()
{
    int index = m_list.GetSelectedIndex();
    if (index < 0 || index >= int(m_rows.size())) return;
    auto entry = m_rows[index].entry;
    if (entry->directory) {
        entry->expanded = !entry->expanded;
        if (entry->expanded && !entry->loading && (!entry->loaded || !entry->error.empty())) m_worker->RequestDirectory(entry);
        RebuildRows(true); return;
    }
    if (!m_pMainFrame->IsStateClosedOrLoaded()) return;
    CAtlList<CString> files; files.AddTail(entry->path.c_str());
    m_pMainFrame->m_wndPlaylistBar.Open(files, false); m_pMainFrame->OpenCurPlaylistItem();
}
void CPlayerFolderBar::OnClick(NMHDR* header, LRESULT* result)
{
    if (reinterpret_cast<NMITEMACTIVATE*>(header)->iItem >= 0) PlaySelection();
    *result = 0;
}
BOOL CPlayerFolderBar::PreTranslateMessage(MSG* message)
{
    if (message->message == WM_KEYDOWN && message->hwnd == m_sort.GetSafeHwnd()) {
        switch (message->wParam) {
            case VK_UP: case VK_DOWN: case VK_HOME: case VK_END:
            case VK_PRIOR: case VK_NEXT: case VK_F4: case VK_RETURN: case VK_ESCAPE:
                m_sort.SendMessage(message->message, message->wParam, message->lParam);
                return TRUE;
        }
    }
    if (message->message == WM_KEYDOWN && message->wParam == VK_TAB && IsDialogMessage(message)) return TRUE;
    return __super::PreTranslateMessage(message);
}
void CPlayerFolderBar::OnDrawItem(int id, LPDRAWITEMSTRUCT item)
{
    if (id == SortID) {
        CDC dc; dc.Attach(item->hDC);
        CRect r(item->rcItem);
        bool selected = (item->itemState & ODS_SELECTED) != 0;
        dc.FillSolidRect(r, AppIsThemeLoaded() ? (selected ? CMPCTheme::ContentSelectedColor : CMPCTheme::ContentBGColor) : GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        dc.SetTextColor(AppIsThemeLoaded() ? CMPCTheme::TextFGColor : GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
        dc.SetBkMode(TRANSPARENT);
        CFont* old = dc.SelectObject(&m_font);
        CString label;
        int index = item->itemID == UINT(-1) ? m_sort.GetCurSel() : int(item->itemID);
        if (index >= 0 && index < m_sort.GetCount()) m_sort.GetLBText(index, label);
        r.DeflateRect(Scale(4), 0);
        dc.DrawText(label, r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        if (item->itemState & ODS_FOCUS) dc.DrawFocusRect(r);
        dc.SelectObject(old); dc.Detach(); return;
    }
    if (id != ListID || item->itemID >= m_rows.size()) { __super::OnDrawItem(id, item); return; }
    const auto& row = m_rows[item->itemID]; auto& entry = *row.entry;
    CDC dc; dc.Attach(item->hDC); CRect r(item->rcItem);
    bool selected = (item->itemState & ODS_SELECTED) != 0;
    COLORREF bg = AppIsThemeLoaded() ? (selected ? CMPCTheme::ContentSelectedColor : CMPCTheme::ContentBGColor) : GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW);
    dc.FillSolidRect(r, bg); dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(AppIsThemeLoaded() ? CMPCTheme::TextFGColor : GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
    CFont* oldFont = dc.SelectObject(&m_font);
    int indent = Scale(std::min(row.depth, 5) * 12);
    if (entry.directory) {
        CRect text(r); text.left += Scale(8) + indent; text.right -= Scale(8);
        CString label = entry.expanded ? L"▾  " : L"▸  "; label += entry.name.c_str();
        if (entry.loading) label += L"  正在读取…";
        else if (!entry.error.empty()) label += L"  无法读取";
        else if (entry.expanded && entry.loaded && entry.children.empty()) label += L"  空";
        dc.DrawText(label, text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else {
        // Painting uses only resident bitmaps. File IO and image decoding run on the worker.
        CRect thumb(r.left + Scale(8) + indent, r.top + Scale(8), r.left + Scale(152) + indent, r.top + Scale(89));
        dc.FillSolidRect(thumb, RGB(24, 24, 24));
        auto image = m_images.find(entry.path);
        if (image != m_images.end() && image->second->bitmap) {
            image->second->lastUsed = GetTickCount64();
            CDC memory; memory.CreateCompatibleDC(&dc);
            HGDIOBJ old = ::SelectObject(memory, image->second->bitmap);
            BITMAP info; GetObject(image->second->bitmap, sizeof(info), &info);
            dc.SetStretchBltMode(COLORONCOLOR);
            dc.StretchBlt(thumb.left, thumb.top, thumb.Width(), thumb.Height(), &memory, 0, 0, info.bmWidth, info.bmHeight, SRCCOPY);
            ::SelectObject(memory, old);
        } else {
            COLORREF old = dc.SetTextColor(RGB(190, 190, 190));
            dc.DrawText(L"视频", thumb, DT_CENTER | DT_VCENTER | DT_SINGLELINE); dc.SetTextColor(old);
        }
        CRect text(r.left + Scale(164) + indent, r.top + Scale(12), r.right - Scale(8), r.bottom - Scale(8));
        dc.DrawText(entry.name.c_str(), text, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (item->itemState & ODS_FOCUS) { CRect focus(r); focus.DeflateRect(1, 1); dc.DrawFocusRect(focus); }
    dc.SelectObject(oldFont); dc.Detach();
}
HBRUSH CPlayerFolderBar::OnCtlColor(CDC* dc, CWnd* wnd, UINT type)
{
    if (AppIsThemeLoaded() && type == CTLCOLOR_STATIC) {
        dc->SetTextColor(CMPCTheme::TextFGColor); dc->SetBkColor(CMPCTheme::WindowBGColor);
        static CBrush brush; brush.DeleteObject(); brush.CreateSolidBrush(CMPCTheme::WindowBGColor); return brush;
    }
    return __super::OnCtlColor(dc, wnd, type);
}
