#pragma once

#include "CMPCThemePlayerBar.h"
#include "CMPCThemeButton.h"
#include "CMPCThemeComboBox.h"
#include "CMPCThemeScrollBar.h"
#include "FolderBrowserModel.h"
#include <map>

class CFolderVideoList : public CWnd
{
    CMPCThemeScrollBar m_scrollbar;
    std::vector<std::unique_ptr<CButton>> m_buttons;
    std::vector<int> m_buttonIndices;
    std::vector<FolderViewRow> m_rows;
    FolderRowLayout m_layout;
    int m_videoHeight = 104, m_folderHeight = 36, m_offset = 0, m_selected = -1, m_wheelRemainder = 0;
    void LayoutRows();
    void ScrollTo(int offset);
    void SelectItem(int index);
    void ActivateSelection();
public:
    BOOL Create(const CRect& rect, CWnd* parent, UINT id);
    void SetRowHeights(int video, int folder) { m_videoHeight = video; m_folderHeight = folder; }
    void SetRows(std::vector<FolderViewRow> rows, bool preservePosition);
    int GetSelectedIndex() const { return m_selected; }
    std::vector<int> VisibleIndices() const;
    void RedrawItems(int first, int last);
    BOOL PreTranslateMessage(MSG* message) override;
protected:
    DECLARE_MESSAGE_MAP()
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC*) { return TRUE; }
    afx_msg void OnSize(UINT type, int cx, int cy);
    afx_msg void OnVScroll(UINT code, UINT pos, CScrollBar* bar);
    afx_msg BOOL OnMouseWheel(UINT flags, short delta, CPoint point);
    afx_msg void OnDrawItem(int id, LPDRAWITEMSTRUCT item);
    afx_msg void OnRowClicked(UINT id);
};

struct FolderBrowserWorker;
struct FolderThumbnail;
class CPlayerFolderBar : public CMPCThemePlayerBar
{
    CMPCThemeButton m_choose, m_recent, m_refresh, m_collapse;
    CMPCThemeComboBox m_sort;
    CStatic m_path, m_status;
    CFolderVideoList m_list;
    CFont m_font;
    CString m_folder, m_lastFolder;
    std::shared_ptr<FolderEntry> m_root;
    std::vector<FolderViewRow> m_rows;
    std::shared_ptr<FolderBrowserWorker> m_worker;
    std::map<std::wstring, std::shared_ptr<FolderThumbnail>> m_images;
    std::set<std::wstring> m_failedImages;
    int m_scale = 96;
    int Scale(int value) const;
    void Browse(const CString& folder);
    void PlaySelection();
    void StopWorker();
    void RebuildRows(bool preservePosition);
    void RequestVisibleImages();
public:
    explicit CPlayerFolderBar(CMainFrame* frame);
    ~CPlayerFolderBar() override;
    BOOL Create(CWnd* parent, UINT dockID);
    void ReloadTranslatableResources() override;
    void LoadState(CFrameWnd* parent) override;
    void SaveState() override;
    void OnUpdateCmdUI(CFrameWnd*, BOOL) override {}
    BOOL PreTranslateMessage(MSG* message) override;
protected:
    DECLARE_MESSAGE_MAP()
    afx_msg void OnChoose();
    afx_msg void OnRecent();
    afx_msg void OnRefresh();
    afx_msg void OnSortChanged();
    afx_msg void OnCollapse();
    afx_msg void OnSize(UINT type, int cx, int cy);
    afx_msg void OnTimer(UINT_PTR id);
    afx_msg void OnDestroy();
    afx_msg void OnClick(NMHDR* header, LRESULT* result);
    afx_msg void OnDrawItem(int id, LPDRAWITEMSTRUCT item);
    afx_msg HBRUSH OnCtlColor(CDC* dc, CWnd* wnd, UINT type);
};
