// ==============================================================================
// ui/ExportDialog.cpp
// ==============================================================================
#include "ExportDialog.h"

#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kMaxExportDimension = 100000;

QStringList builtInPresetNames()
{
    return {
        QStringLiteral("Web"),
        QStringLiteral("Print"),
        QStringLiteral("Full Quality"),
        QStringLiteral("Social Media"),
    };
}

QFrame* makeCard(QWidget* parent)
{
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("exportCard"));
    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(14, 12, 14, 12);
    lay->setSpacing(10);
    return card;
}

QLabel* makeSectionTitle(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    QFont font = label->font();
    font.setBold(true);
    font.setPointSize(font.pointSize() + 1);
    label->setFont(font);
    label->setStyleSheet(QStringLiteral("color: #E7E9EE;"));
    return label;
}

QString cleanBaseName(QString name)
{
    name = name.trimmed();
    if (name.isEmpty())
        return QStringLiteral("export");
    return QFileInfo(name).completeBaseName().trimmed().isEmpty()
        ? name
        : QFileInfo(name).completeBaseName().trimmed();
}

} // namespace

QString ExportDialog::Options::extension() const
{
    if (format.compare(QStringLiteral("JPG"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("jpg");
    if (format.compare(QStringLiteral("TIFF"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("tiff");
    if (format.compare(QStringLiteral("WEBP"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("webp");
    return QStringLiteral("png");
}

QByteArray ExportDialog::Options::imageFormat() const
{
    return extension().toLatin1();
}

QString ExportDialog::Options::outputPath() const
{
    QString name = fileName.trimmed();
    if (name.isEmpty())
        name = QStringLiteral("export");

    const QString ext = extension();
    QFileInfo fileInfo(name);
    if (fileInfo.suffix().isEmpty()) {
        name += QLatin1Char('.') + ext;
    } else if (fileInfo.suffix().compare(ext, Qt::CaseInsensitive) != 0) {
        name = cleanBaseName(name) + QLatin1Char('.') + ext;
    }

    return QDir(outputFolder).filePath(name);
}

ExportDialog::ExportDialog(const QString& defaultFolder,
                           const QString& defaultFileName,
                           const QSize& sourceSize,
                           QWidget* parent)
    : QDialog(parent)
    , m_sourceSize(sourceSize.isValid() ? sourceSize : QSize(1, 1))
{
    setWindowTitle(tr("Export"));
    setModal(true);
    resize(560, 680);
    buildUi(defaultFolder, defaultFileName);
    loadPresetNames();
    updateQualityState();
    updateResizeState();
}

ExportDialog::Options ExportDialog::options() const
{
    Options out;
    out.format = m_formatCombo->currentText();
    out.quality = m_qualitySlider->value();
    out.resize = m_resizeCheck->isChecked();
    out.width = m_widthSpin->value();
    out.height = m_heightSpin->value();
    out.preserveAspectRatio = m_preserveAspectCheck->isChecked();
    out.colorSpace = m_colorSpaceCombo->currentText();
    out.includeMetadata = m_includeMetadataCheck->isChecked();
    out.outputFolder = QDir::fromNativeSeparators(m_outputFolderEdit->text().trimmed());
    out.fileName = m_fileNameEdit->text().trimmed();
    return out;
}

void ExportDialog::accept()
{
    const Options opts = options();
    if (opts.outputFolder.isEmpty() || !QFileInfo(opts.outputFolder).isDir()) {
        QMessageBox::warning(this, tr("Export"), tr("Choose a valid output folder."));
        return;
    }
    if (opts.fileName.isEmpty()) {
        QMessageBox::warning(this, tr("Export"), tr("Enter a filename."));
        return;
    }
    if (QFileInfo::exists(opts.outputPath())) {
        const auto reply = QMessageBox::question(
            this,
            tr("Export"),
            tr("Replace existing file?\n%1").arg(opts.outputPath()),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (reply != QMessageBox::Yes)
            return;
    }
    QDialog::accept();
}

void ExportDialog::buildUi(const QString& defaultFolder, const QString& defaultFileName)
{
    setStyleSheet(QStringLiteral(R"(
        QDialog {
            background: #0E0F12;
            color: #E7E9EE;
        }
        QFrame#exportCard {
            background: #16181D;
            border: 1px solid #2A2D35;
            border-radius: 10px;
        }
        QLabel {
            color: #DDE0E7;
            background: transparent;
        }
        QLineEdit, QComboBox, QSpinBox {
            background: #111318;
            color: #E7E9EE;
            border: 1px solid #2A2D35;
            border-radius: 7px;
            padding: 5px 8px;
        }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus {
            border-color: #CCFF00;
        }
        QPushButton {
            background: #1E2026;
            color: #E7E9EE;
            border: 1px solid #2A2D35;
            border-radius: 7px;
            padding: 6px 12px;
        }
        QPushButton:hover {
            background: #24272E;
            border-color: #3D424E;
            color: #FFFFFF;
        }
        QPushButton#primaryExportButton {
            background: #CCFF00;
            color: #101114;
            border-color: #CCFF00;
            font-weight: 700;
        }
        QCheckBox {
            color: #DDE0E7;
            spacing: 8px;
        }
        QSlider::groove:horizontal {
            height: 3px;
            background: #2A2D35;
            border-radius: 2px;
        }
        QSlider::sub-page:horizontal {
            background: #CCFF00;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            width: 14px;
            height: 14px;
            margin: -6px 0;
            border-radius: 7px;
            background: #8B929D;
            border: 1px solid #B0B7C2;
        }
    )"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto* title = new QLabel(tr("Export Image"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 4);
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setStyleSheet(QStringLiteral("color: #FFFFFF;"));
    root->addWidget(title);

    {
        auto* card = makeCard(this);
        auto* lay = static_cast<QVBoxLayout*>(card->layout());
        lay->addWidget(makeSectionTitle(tr("Export Preset"), card));

        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);

        m_presetCombo = new QComboBox(card);
        row->addWidget(m_presetCombo, 1);

        m_loadPresetBtn = new QPushButton(tr("Load Export Preset"), card);
        m_savePresetBtn = new QPushButton(tr("Save Export Preset"), card);
        row->addWidget(m_loadPresetBtn);
        row->addWidget(m_savePresetBtn);
        lay->addLayout(row);
        root->addWidget(card);

        connect(m_loadPresetBtn, &QPushButton::clicked, this, [this]() {
            applyPreset(m_presetCombo->currentText());
        });
        connect(m_savePresetBtn, &QPushButton::clicked, this, [this]() {
            bool ok = false;
            const QString name = QInputDialog::getText(
                this,
                tr("Save Export Preset"),
                tr("Preset name"),
                QLineEdit::Normal,
                m_presetCombo->currentText(),
                &ok).trimmed();
            if (!ok || name.isEmpty())
                return;
            savePreset(name, options());
            loadPresetNames();
            m_presetCombo->setCurrentText(name);
        });
    }

    {
        auto* card = makeCard(this);
        auto* lay = static_cast<QVBoxLayout*>(card->layout());
        lay->addWidget(makeSectionTitle(tr("File Settings"), card));

        auto* form = new QFormLayout();
        form->setContentsMargins(0, 0, 0, 0);
        form->setSpacing(8);

        m_formatCombo = new QComboBox(card);
        m_formatCombo->addItems(QStringList{
            tr("JPG"),
            tr("PNG"),
            tr("TIFF"),
            tr("WEBP"),
        });
        const QString suffix = QFileInfo(defaultFileName).suffix().toUpper();
        if (suffix == QStringLiteral("JPG") || suffix == QStringLiteral("JPEG"))
            m_formatCombo->setCurrentText(QStringLiteral("JPG"));
        else if (suffix == QStringLiteral("TIF") || suffix == QStringLiteral("TIFF"))
            m_formatCombo->setCurrentText(QStringLiteral("TIFF"));
        else if (suffix == QStringLiteral("WEBP"))
            m_formatCombo->setCurrentText(QStringLiteral("WEBP"));
        else
            m_formatCombo->setCurrentText(QStringLiteral("PNG"));
        form->addRow(tr("Format"), m_formatCombo);

        auto* qualityRow = new QWidget(card);
        auto* qualityLay = new QHBoxLayout(qualityRow);
        qualityLay->setContentsMargins(0, 0, 0, 0);
        qualityLay->setSpacing(8);
        m_qualitySlider = new QSlider(Qt::Horizontal, qualityRow);
        m_qualitySlider->setRange(1, 100);
        m_qualitySlider->setValue(90);
        m_qualityValue = new QLabel(QStringLiteral("90"), qualityRow);
        m_qualityValue->setMinimumWidth(32);
        m_qualityValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        qualityLay->addWidget(m_qualitySlider, 1);
        qualityLay->addWidget(m_qualityValue);
        form->addRow(tr("Quality"), qualityRow);

        m_colorSpaceCombo = new QComboBox(card);
        m_colorSpaceCombo->addItems(QStringList{
            tr("sRGB"),
            tr("Display P3 placeholder"),
            tr("Adobe RGB placeholder"),
        });
        form->addRow(tr("Color Space"), m_colorSpaceCombo);

        m_includeMetadataCheck = new QCheckBox(tr("Include metadata"), card);
        m_includeMetadataCheck->setChecked(true);
        form->addRow(tr("Metadata"), m_includeMetadataCheck);

        lay->addLayout(form);
        root->addWidget(card);

        connect(m_formatCombo, &QComboBox::currentTextChanged, this, [this]() {
            updateQualityState();
            updateFileNameExtension();
        });
        connect(m_qualitySlider, &QSlider::valueChanged, this, [this](int value) {
            m_qualityValue->setText(QString::number(value));
        });
    }

    {
        auto* card = makeCard(this);
        auto* lay = static_cast<QVBoxLayout*>(card->layout());
        lay->addWidget(makeSectionTitle(tr("Image Sizing"), card));

        auto* form = new QFormLayout();
        form->setContentsMargins(0, 0, 0, 0);
        form->setSpacing(8);

        m_resizeCheck = new QCheckBox(tr("Resize"), card);
        form->addRow(QString(), m_resizeCheck);

        m_widthSpin = new QSpinBox(card);
        m_widthSpin->setRange(1, kMaxExportDimension);
        m_widthSpin->setValue(std::max(1, m_sourceSize.width()));
        form->addRow(tr("Width"), m_widthSpin);

        m_heightSpin = new QSpinBox(card);
        m_heightSpin->setRange(1, kMaxExportDimension);
        m_heightSpin->setValue(std::max(1, m_sourceSize.height()));
        form->addRow(tr("Height"), m_heightSpin);

        m_preserveAspectCheck = new QCheckBox(tr("Preserve aspect ratio"), card);
        m_preserveAspectCheck->setChecked(true);
        form->addRow(QString(), m_preserveAspectCheck);

        lay->addLayout(form);
        root->addWidget(card);

        connect(m_resizeCheck, &QCheckBox::toggled, this,
                &ExportDialog::updateResizeState);
        connect(m_widthSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &ExportDialog::syncHeightFromWidth);
        connect(m_heightSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &ExportDialog::syncWidthFromHeight);
    }

    {
        auto* card = makeCard(this);
        auto* lay = static_cast<QVBoxLayout*>(card->layout());
        lay->addWidget(makeSectionTitle(tr("Destination"), card));

        auto* form = new QFormLayout();
        form->setContentsMargins(0, 0, 0, 0);
        form->setSpacing(8);

        auto* folderRow = new QWidget(card);
        auto* folderLay = new QHBoxLayout(folderRow);
        folderLay->setContentsMargins(0, 0, 0, 0);
        folderLay->setSpacing(8);
        m_outputFolderEdit = new QLineEdit(QDir::fromNativeSeparators(defaultFolder), folderRow);
        m_outputFolderBtn = new QPushButton(tr("Choose"), folderRow);
        folderLay->addWidget(m_outputFolderEdit, 1);
        folderLay->addWidget(m_outputFolderBtn);
        form->addRow(tr("Output Folder"), folderRow);

        m_fileNameEdit = new QLineEdit(defaultFileName, card);
        form->addRow(tr("Filename"), m_fileNameEdit);

        lay->addLayout(form);
        root->addWidget(card);

        connect(m_outputFolderBtn, &QPushButton::clicked, this, [this]() {
            const QString folder = QFileDialog::getExistingDirectory(
                this, tr("Output Folder"), m_outputFolderEdit->text());
            if (!folder.isEmpty())
                m_outputFolderEdit->setText(QDir::fromNativeSeparators(folder));
        });
    }

    auto* buttons = new QDialogButtonBox(this);
    auto* cancelBtn = buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    auto* exportBtn = buttons->addButton(tr("Export"), QDialogButtonBox::AcceptRole);
    exportBtn->setObjectName(QStringLiteral("primaryExportButton"));
    cancelBtn->setCursor(Qt::PointingHandCursor);
    exportBtn->setCursor(Qt::PointingHandCursor);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &ExportDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &ExportDialog::reject);

    updateFileNameExtension();
}

void ExportDialog::loadPresetNames()
{
    const QString current = m_presetCombo ? m_presetCombo->currentText() : QString();
    m_presetCombo->clear();
    m_presetCombo->addItems(builtInPresetNames());

    QSettings settings;
    settings.beginGroup(QStringLiteral("export/presets"));
    for (const QString& key : settings.childGroups()) {
        settings.beginGroup(key);
        const QString name = settings.value(QStringLiteral("name")).toString();
        settings.endGroup();
        if (!name.isEmpty() && m_presetCombo->findText(name) < 0)
            m_presetCombo->addItem(name);
    }
    settings.endGroup();

    if (!current.isEmpty() && m_presetCombo->findText(current) >= 0)
        m_presetCombo->setCurrentText(current);
}

void ExportDialog::applyPreset(const QString& name)
{
    Options preset;
    if (builtInPresetNames().contains(name)) {
        preset = builtInPreset(name, m_sourceSize);
    } else {
        QSettings settings;
        settings.beginGroup(QStringLiteral("export/presets/%1").arg(presetKey(name)));
        if (!settings.contains(QStringLiteral("format"))) {
            settings.endGroup();
            return;
        }
        preset.format = settings.value(QStringLiteral("format"), QStringLiteral("PNG")).toString();
        preset.quality = settings.value(QStringLiteral("quality"), 90).toInt();
        preset.resize = settings.value(QStringLiteral("resize"), false).toBool();
        preset.width = settings.value(QStringLiteral("width"), m_sourceSize.width()).toInt();
        preset.height = settings.value(QStringLiteral("height"), m_sourceSize.height()).toInt();
        preset.preserveAspectRatio = settings.value(QStringLiteral("preserveAspectRatio"), true).toBool();
        preset.colorSpace = settings.value(QStringLiteral("colorSpace"), QStringLiteral("sRGB")).toString();
        preset.includeMetadata = settings.value(QStringLiteral("includeMetadata"), true).toBool();
        settings.endGroup();
    }

    m_formatCombo->setCurrentText(preset.format);
    m_qualitySlider->setValue(std::clamp(preset.quality, 1, 100));
    m_resizeCheck->setChecked(preset.resize);
    m_preserveAspectCheck->setChecked(preset.preserveAspectRatio);
    m_syncingSize = true;
    m_widthSpin->setValue(std::clamp(preset.width, 1, kMaxExportDimension));
    m_heightSpin->setValue(std::clamp(preset.height, 1, kMaxExportDimension));
    m_syncingSize = false;
    m_colorSpaceCombo->setCurrentText(preset.colorSpace);
    m_includeMetadataCheck->setChecked(preset.includeMetadata);
    updateQualityState();
    updateResizeState();
    updateFileNameExtension();
}

void ExportDialog::savePreset(const QString& name, const Options& options)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("export/presets/%1").arg(presetKey(name)));
    settings.setValue(QStringLiteral("name"), name);
    settings.setValue(QStringLiteral("format"), options.format);
    settings.setValue(QStringLiteral("quality"), options.quality);
    settings.setValue(QStringLiteral("resize"), options.resize);
    settings.setValue(QStringLiteral("width"), options.width);
    settings.setValue(QStringLiteral("height"), options.height);
    settings.setValue(QStringLiteral("preserveAspectRatio"), options.preserveAspectRatio);
    settings.setValue(QStringLiteral("colorSpace"), options.colorSpace);
    settings.setValue(QStringLiteral("includeMetadata"), options.includeMetadata);
    settings.endGroup();
}

void ExportDialog::updateQualityState()
{
    const QString format = m_formatCombo->currentText();
    const bool enabled = format == QStringLiteral("JPG")
        || format == QStringLiteral("WEBP");
    m_qualitySlider->setEnabled(enabled);
    m_qualityValue->setEnabled(enabled);
}

void ExportDialog::updateResizeState()
{
    const bool enabled = m_resizeCheck->isChecked();
    m_widthSpin->setEnabled(enabled);
    m_heightSpin->setEnabled(enabled);
    m_preserveAspectCheck->setEnabled(enabled);
}

void ExportDialog::updateFileNameExtension()
{
    if (!m_fileNameEdit || !m_formatCombo)
        return;
    const QString current = m_fileNameEdit->text().trimmed();
    if (current.isEmpty())
        return;

    Options opts = options();
    opts.fileName = current;
    m_fileNameEdit->setText(QFileInfo(opts.outputPath()).fileName());
}

void ExportDialog::syncHeightFromWidth(int width)
{
    if (m_syncingSize || !m_resizeCheck->isChecked() || !m_preserveAspectCheck->isChecked())
        return;
    m_syncingSize = true;
    const double ratio = static_cast<double>(m_sourceSize.height())
        / static_cast<double>(std::max(1, m_sourceSize.width()));
    m_heightSpin->setValue(std::clamp(static_cast<int>(std::round(width * ratio)),
                                      1, kMaxExportDimension));
    m_syncingSize = false;
}

void ExportDialog::syncWidthFromHeight(int height)
{
    if (m_syncingSize || !m_resizeCheck->isChecked() || !m_preserveAspectCheck->isChecked())
        return;
    m_syncingSize = true;
    const double ratio = static_cast<double>(m_sourceSize.width())
        / static_cast<double>(std::max(1, m_sourceSize.height()));
    m_widthSpin->setValue(std::clamp(static_cast<int>(std::round(height * ratio)),
                                     1, kMaxExportDimension));
    m_syncingSize = false;
}

ExportDialog::Options ExportDialog::builtInPreset(const QString& name,
                                                  const QSize& sourceSize)
{
    Options preset;
    preset.width = std::max(1, sourceSize.width());
    preset.height = std::max(1, sourceSize.height());
    preset.preserveAspectRatio = true;
    preset.colorSpace = QStringLiteral("sRGB");

    if (name == QStringLiteral("Web")) {
        preset.format = QStringLiteral("JPG");
        preset.quality = 82;
        preset.resize = true;
        preset.width = 2048;
        preset.height = 2048;
        preset.includeMetadata = false;
    } else if (name == QStringLiteral("Print")) {
        preset.format = QStringLiteral("TIFF");
        preset.quality = 100;
        preset.resize = false;
        preset.includeMetadata = true;
    } else if (name == QStringLiteral("Full Quality")) {
        preset.format = QStringLiteral("PNG");
        preset.quality = 100;
        preset.resize = false;
        preset.includeMetadata = true;
    } else if (name == QStringLiteral("Social Media")) {
        preset.format = QStringLiteral("JPG");
        preset.quality = 85;
        preset.resize = true;
        preset.width = 1600;
        preset.height = 1600;
        preset.includeMetadata = false;
    }
    return preset;
}

QString ExportDialog::presetKey(const QString& name)
{
    return QString::fromLatin1(name.toUtf8().toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
