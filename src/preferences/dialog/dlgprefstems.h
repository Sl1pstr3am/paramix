#pragma once

#include "preferences/dialog/dlgpreferencepage.h"
#include "preferences/dialog/ui_dlgprefstemsdlg.h"
#include "preferences/usersettings.h"
#include "stems/stemfileformat.h"

class DlgPrefStems : public DlgPreferencePage, public Ui::DlgStemsDlg {
    Q_OBJECT

  public:
    DlgPrefStems(QWidget* pParent, UserSettingsPointer pConfig);
    ~DlgPrefStems() override;

    QUrl helpUrl() const override;

  public slots:
    void slotApply() override;
    void slotUpdate() override;
    void slotResetToDefaults() override;

  private slots:
    void slotBrowseModel();
    void slotFormatChanged(int index);

  private:
    void updateGui();

    const UserSettingsPointer m_pConfig;
    mixxx::stems::StemExportSettings m_settings;
    QString m_modelPath;
};
