#include "preferences/dialog/dlgprefstems.h"

#include <QFileDialog>
#include <QFileInfo>

#include "moc_dlgprefstems.cpp"
#include "preferences/configobject.h"

namespace {
constexpr char kGroup[] = "[Stems]";
constexpr char kModelPathKey[] = "ModelPath";
} // namespace

DlgPrefStems::DlgPrefStems(QWidget* pParent, UserSettingsPointer pConfig)
        : DlgPreferencePage(pParent),
          m_pConfig(std::move(pConfig)) {
    setupUi(this);
    connect(pushButtonBrowseModel,
            &QPushButton::clicked,
            this,
            &DlgPrefStems::slotBrowseModel);
    connect(comboBoxFormat,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &DlgPrefStems::slotFormatChanged);
    setScrollSafeGuard(comboBoxFormat);
    setScrollSafeGuard(spinBoxAacBitrate);

    slotUpdate();
}

DlgPrefStems::~DlgPrefStems() = default;

QUrl DlgPrefStems::helpUrl() const {
    return QUrl();
}

void DlgPrefStems::slotUpdate() {
    m_settings = mixxx::stems::readStemExportSettings(m_pConfig);
    m_modelPath = m_pConfig->getValue(ConfigKey(kGroup, kModelPathKey), QString());
    updateGui();
}

void DlgPrefStems::slotApply() {
    m_settings.aacBitrateKbps = spinBoxAacBitrate->value();
    m_modelPath = lineEditModelPath->text().trimmed();
    mixxx::stems::writeStemExportSettings(m_pConfig, m_settings);
    m_pConfig->setValue(ConfigKey(kGroup, kModelPathKey), m_modelPath);
}

void DlgPrefStems::slotResetToDefaults() {
    m_settings = mixxx::stems::StemExportSettings();
    m_modelPath.clear();
    updateGui();
}

void DlgPrefStems::slotBrowseModel() {
    const QString path = QFileDialog::getOpenFileName(this,
            tr("Select Stem Separation Model"),
            QFileInfo(m_modelPath).absolutePath(),
            tr("ONNX models (*.onnx *.json);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    m_modelPath = path;
    updateGui();
}

void DlgPrefStems::slotFormatChanged(int index) {
    m_settings.format = index == 1 ? mixxx::stems::StemFileFormat::Alac
            : index == 2           ? mixxx::stems::StemFileFormat::Aac
                                   : mixxx::stems::StemFileFormat::Flac;
    updateGui();
}

void DlgPrefStems::updateGui() {
    switch (m_settings.format) {
    case mixxx::stems::StemFileFormat::Alac:
        comboBoxFormat->setCurrentIndex(1);
        break;
    case mixxx::stems::StemFileFormat::Aac:
        comboBoxFormat->setCurrentIndex(2);
        break;
    case mixxx::stems::StemFileFormat::Flac:
        comboBoxFormat->setCurrentIndex(0);
        break;
    }
    spinBoxAacBitrate->setValue(m_settings.aacBitrateKbps);
    spinBoxAacBitrate->setEnabled(
            m_settings.format == mixxx::stems::StemFileFormat::Aac);
    labelAacBitrate->setEnabled(m_settings.format == mixxx::stems::StemFileFormat::Aac);
    lineEditModelPath->setText(m_modelPath);
}
