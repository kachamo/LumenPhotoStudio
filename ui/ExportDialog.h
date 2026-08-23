// ==============================================================================
// ui/ExportDialog.h
// ==============================================================================
#pragma once

#include <QByteArray>
#include <QColorSpace>
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

        // Bits per colour channel in the written file: 8 or 16.
        //
        // This is the value that will actually be used, not a wish. The
        // dialog forces it back to 8 whenever the selected format cannot
        // carry 16 bits (JPEG by specification, or any format whose Qt
        // plugin is missing or downconverts), so the UI never claims a depth
        // the export will not deliver.
        int bitDepth = 8;

        bool resize = false;
        int width = 0;
        int height = 0;
        bool preserveAspectRatio = true;

        // Stable, untranslated identifier for the output colour space:
        // "sRGB", "AdobeRGB", "DisplayP3" or "ProPhotoRGB". Deliberately not
        // the combo's display text — that is translated, and this value is
        // persisted in QSettings where a translated string would rot.
        QString colorSpace = QStringLiteral("sRGB");

        bool includeMetadata = true;
        QString outputFolder;
        QString fileName;

        QString extension() const;
        QByteArray imageFormat() const;
        QString outputPath() const;

        // The QColorSpace the exported pixels must be CONVERTED into (not
        // merely tagged with). Always valid; unrecognized identifiers, and
        // the legacy "... placeholder" strings that older saved presets
        // contain, resolve to sRGB.
        QColorSpace targetColorSpace() const;

        // True when the chosen space is materially wider than sRGB, i.e.
        // when 8-bit output risks visible banding.
        bool isWideGamut() const;
    };

    // Does this Qt build genuinely write `imageFormat` at 16 bits per
    // channel? Answered by round-tripping a tiny Format_RGBA64 image through
    // the real codec and checking the depth that comes back — asking
    // QImageWriter::supportedImageFormats() only proves the codec exists, and
    // the JPEG handler will happily accept a 64-bit image and quietly write
    // 8 bits. Result is cached per format; call from the GUI thread only.
    static bool formatSupports16Bit(const QByteArray& imageFormat);

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
    void updateBitDepthState();
    void updateColorManagementWarning();
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
    QComboBox* m_bitDepthCombo = nullptr;
    QSlider* m_qualitySlider = nullptr;
    QLabel* m_qualityValue = nullptr;
    QCheckBox* m_resizeCheck = nullptr;
    QSpinBox* m_widthSpin = nullptr;
    QSpinBox* m_heightSpin = nullptr;
    QCheckBox* m_preserveAspectCheck = nullptr;
    QComboBox* m_colorSpaceCombo = nullptr;
    QLabel* m_colorManagementNote = nullptr;
    QCheckBox* m_includeMetadataCheck = nullptr;
    QLineEdit* m_outputFolderEdit = nullptr;
    QPushButton* m_outputFolderBtn = nullptr;
    QLineEdit* m_fileNameEdit = nullptr;
};
