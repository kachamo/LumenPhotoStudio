// ==============================================================================
// ui/ExportDialog.h
// ==============================================================================
#pragma once

#include <QByteArray>
#include <QDialog>
#include <QSize>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;

class ExportDialog final : public QDialog
{
    Q_OBJECT

public:
    struct Options {
        QString format = QStringLiteral("PNG");
        int quality = 90;
        bool resize = false;
        int width = 0;
        int height = 0;
        bool preserveAspectRatio = true;
        QString colorSpace = QStringLiteral("sRGB");
        bool includeMetadata = true;
        QString outputFolder;
        QString fileName;

        QString extension() const;
        QByteArray imageFormat() const;
        QString outputPath() const;
    };

    explicit ExportDialog(const QString& defaultFolder,
                          const QString& defaultFileName,
                          const QSize& sourceSize,
                          QWidget* parent = nullptr);

    Options options() const;

protected:
    void accept() override;

private:
    void buildUi(const QString& defaultFolder, const QString& defaultFileName);
    void loadPresetNames();
    void applyPreset(const QString& name);
    void savePreset(const QString& name, const Options& options);
    void updateQualityState();
    void updateResizeState();
    void updateFileNameExtension();
    void syncHeightFromWidth(int width);
    void syncWidthFromHeight(int height);

    static Options builtInPreset(const QString& name, const QSize& sourceSize);
    static QString presetKey(const QString& name);

    QSize m_sourceSize;
    bool m_syncingSize = false;

    QComboBox* m_presetCombo = nullptr;
    QPushButton* m_loadPresetBtn = nullptr;
    QPushButton* m_savePresetBtn = nullptr;

    QComboBox* m_formatCombo = nullptr;
    QSlider* m_qualitySlider = nullptr;
    QLabel* m_qualityValue = nullptr;
    QCheckBox* m_resizeCheck = nullptr;
    QSpinBox* m_widthSpin = nullptr;
    QSpinBox* m_heightSpin = nullptr;
    QCheckBox* m_preserveAspectCheck = nullptr;
    QComboBox* m_colorSpaceCombo = nullptr;
    QCheckBox* m_includeMetadataCheck = nullptr;
    QLineEdit* m_outputFolderEdit = nullptr;
    QPushButton* m_outputFolderBtn = nullptr;
    QLineEdit* m_fileNameEdit = nullptr;
};
