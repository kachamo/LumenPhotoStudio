// ==============================================================================
// ui/WelcomeScreenWidget.cpp
// ==============================================================================
#include "WelcomeScreenWidget.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QMimeData>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QStyle>
#include <QSignalBlocker>
#include <QStringList>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>

#include <functional>
#include <utility>

namespace {

const QStringList& supportedExts()
{
    static const QStringList exts = {
        QStringLiteral("png"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("bmp"),
        QStringLiteral("tif"),
        QStringLiteral("tiff"),
        QStringLiteral("webp"),
        QStringLiteral("cr2"),
        QStringLiteral("cr3"),
        QStringLiteral("nef"),
        QStringLiteral("arw"),
        QStringLiteral("dng"),
        QStringLiteral("raf"),
        QStringLiteral("orf"),
        QStringLiteral("rw2"),
    };
    return exts;
}

QLabel* makeLabel(const QString& text, QWidget* parent,
                  const char* objectName = nullptr)
{
    auto* label = new QLabel(text, parent);
    if (objectName) label->setObjectName(QString::fromLatin1(objectName));
    return label;
}

QPushButton* makeSideButton(const QString& text, QWidget* parent,
                            const char* objectName)
{
    auto* button = new QPushButton(text, parent);
    button->setObjectName(QString::fromLatin1(objectName));
    button->setMinimumHeight(52);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QToolButton* makeOtherButton(const QString& text, QWidget* parent)
{
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("welcomeOtherButton"));
    button->setText(text);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumHeight(38);
    return button;
}

class RecentItemFrame final : public QFrame
{
public:
    explicit RecentItemFrame(QWidget* parent = nullptr)
        : QFrame(parent)
    {}

    std::function<void()> activated;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        QFrame::mouseReleaseEvent(event);
        if (event->button() == Qt::LeftButton && activated)
            activated();
    }
};

QFrame* makeRecentItem(QWidget* parent,
                       const QString& fileName,
                       const QString& time,
                       const QString& tag,
                       int index,
                       const QString& path = QString(),
                       std::function<void()> activated = {})
{
    auto* item = new RecentItemFrame(parent);
    item->setObjectName(QStringLiteral("welcomeRecentItem"));
    item->setProperty("tone", index % 5);
    if (!path.isEmpty()) {
        item->setToolTip(path);
        item->setCursor(Qt::PointingHandCursor);
    }
    item->activated = std::move(activated);
    auto* lay = new QVBoxLayout(item);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(7);

    auto* thumb = new QFrame(item);
    thumb->setObjectName(QStringLiteral("welcomeThumb"));
    thumb->setProperty("tone", index % 5);
    thumb->setAttribute(Qt::WA_TransparentForMouseEvents);
    thumb->setMinimumSize(142, 78);
    thumb->setMaximumHeight(92);
    auto* thumbLay = new QVBoxLayout(thumb);
    thumbLay->setContentsMargins(8, 8, 8, 8);
    const bool projectItem = tag.compare(QStringLiteral("PROJECT"),
                                         Qt::CaseInsensitive) == 0;
    QPixmap pix;
    if (!path.isEmpty() && !projectItem)
        pix.load(path);
    if (!pix.isNull()) {
        auto* image = new QLabel(thumb);
        image->setAttribute(Qt::WA_TransparentForMouseEvents);
        image->setAlignment(Qt::AlignCenter);
        image->setPixmap(pix.scaled(142, 78,
                                    Qt::KeepAspectRatioByExpanding,
                                    Qt::SmoothTransformation));
        thumbLay->addWidget(image, 1);
    } else {
        thumbLay->addStretch(1);
        auto* fallback = makeLabel(fileName.left(2).toUpper(), thumb,
                                   "welcomeThumbFallback");
        fallback->setAttribute(Qt::WA_TransparentForMouseEvents);
        fallback->setAlignment(Qt::AlignCenter);
        thumbLay->addWidget(fallback, 0, Qt::AlignCenter);
        thumbLay->addStretch(1);
    }
    if (!tag.isEmpty()) {
        auto* badge = makeLabel(tag, thumb, "welcomeRawBadge");
        badge->setAttribute(Qt::WA_TransparentForMouseEvents);
        badge->setAlignment(Qt::AlignCenter);
        thumbLay->addWidget(badge, 0, Qt::AlignRight | Qt::AlignBottom);
    }
    lay->addWidget(thumb);

    auto* name = makeLabel(fileName, item, "welcomeRecentName");
    name->setAttribute(Qt::WA_TransparentForMouseEvents);
    lay->addWidget(name);
    auto* meta = makeLabel(time, item, "welcomeRecentMeta");
    meta->setAttribute(Qt::WA_TransparentForMouseEvents);
    lay->addWidget(meta);
    return item;
}

QFrame* makeInfoCard(QWidget* parent, const QString& title)
{
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("welcomeInfoCard"));
    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(22, 20, 22, 20);
    lay->setSpacing(16);
    auto* heading = makeLabel(title, card, "welcomeCardTitle");
    lay->addWidget(heading);
    return card;
}

} // namespace

WelcomeScreenWidget::WelcomeScreenWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("welcomeScreen"));
    setAcceptDrops(true);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(14);

    // ---- Left sidebar ------------------------------------------------------
    auto* sidebar = new QFrame(this);
    sidebar->setObjectName(QStringLiteral("welcomeSidebar"));
    sidebar->setMinimumWidth(292);
    sidebar->setMaximumWidth(330);
    auto* sideLay = new QVBoxLayout(sidebar);
    sideLay->setContentsMargins(28, 34, 28, 28);
    sideLay->setSpacing(12);

    auto* brandRow = new QHBoxLayout();
    brandRow->setContentsMargins(0, 0, 0, 20);
    brandRow->setSpacing(12);
    auto* sideLogo = new QLabel(sidebar);
    sideLogo->setObjectName(QStringLiteral("welcomeSideLogo"));
    sideLogo->setPixmap(QIcon(QStringLiteral(":/icons/lumen_logo_512.png")).pixmap(40, 40));
    sideLogo->setFixedSize(44, 44);
    sideLogo->setAlignment(Qt::AlignCenter);
    brandRow->addWidget(sideLogo);
    auto* sideTitle = makeLabel(tr("<span style='color:#CCFF00;font-weight:700'>Lumen</span> Photo Studio"),
                                sidebar, "welcomeSideTitle");
    brandRow->addWidget(sideTitle, 1);
    sideLay->addLayout(brandRow);

    auto* openImageBtn = makeSideButton(tr("  Open Image"), sidebar, "welcomePrimaryButton");
    connect(openImageBtn, &QPushButton::clicked,
            this, &WelcomeScreenWidget::openImageRequested);
    sideLay->addWidget(openImageBtn);

    auto* openProjectBtn = makeSideButton(tr("  Open Project"), sidebar, "welcomeActionButton");
    connect(openProjectBtn, &QPushButton::clicked,
            this, &WelcomeScreenWidget::openProjectRequested);
    sideLay->addWidget(openProjectBtn);

    auto* newProjectBtn = makeSideButton(tr("  New Project        SOON"), sidebar, "welcomeActionButton");
    connect(newProjectBtn, &QPushButton::clicked,
            this, &WelcomeScreenWidget::newProjectRequested);
    sideLay->addWidget(newProjectBtn);

    auto* sep1 = new QFrame(sidebar);
    sep1->setObjectName(QStringLiteral("welcomeSeparator"));
    sep1->setFrameShape(QFrame::HLine);
    sideLay->addSpacing(24);
    sideLay->addWidget(sep1);

    auto* other = makeLabel(tr("OTHER"), sidebar, "welcomeSectionLabel");
    sideLay->addSpacing(12);
    sideLay->addWidget(other);

    const QStringList otherItems = {
        tr("Recent Projects"),
        tr("Preferences"),
        tr("Plugins        SOON"),
        tr("Check for Updates"),
        tr("Help & Documentation"),
        tr("About Lumen"),
    };
    for (const QString& text : otherItems) {
        auto* button = makeOtherButton(text, sidebar);
        if (text.startsWith(tr("Preferences")))
            connect(button, &QToolButton::clicked,
                    this, &WelcomeScreenWidget::preferencesRequested);
        else if (text.startsWith(tr("Plugins")))
            connect(button, &QToolButton::clicked,
                    this, &WelcomeScreenWidget::pluginsRequested);
        sideLay->addWidget(button);
    }

    sideLay->addStretch(1);

    auto* socialSep = new QFrame(sidebar);
    socialSep->setObjectName(QStringLiteral("welcomeSeparator"));
    socialSep->setFrameShape(QFrame::HLine);
    sideLay->addWidget(socialSep);

    auto* socials = new QHBoxLayout();
    socials->setContentsMargins(0, 12, 0, 12);
    socials->setSpacing(18);
    const QStringList socialIcons = { tr("web"), tr("dc"), tr("x"), tr("yt") };
    for (const QString& icon : socialIcons) {
        auto* label = makeLabel(icon, sidebar, "welcomeSocialIcon");
        label->setAlignment(Qt::AlignCenter);
        socials->addWidget(label, 1);
    }
    sideLay->addLayout(socials);
    auto* version = makeLabel(tr("v1.0.0"), sidebar, "welcomeVersion");
    version->setAlignment(Qt::AlignCenter);
    sideLay->addWidget(version);
    root->addWidget(sidebar);

    // ---- Main scene shell --------------------------------------------------
    auto* shell = new QFrame(this);
    shell->setObjectName(QStringLiteral("welcomeMainShell"));
    auto* shadow = new QGraphicsDropShadowEffect(shell);
    shadow->setBlurRadius(44);
    shadow->setOffset(0, 18);
    shadow->setColor(QColor(0, 0, 0, 145));
    shell->setGraphicsEffect(shadow);
    auto* shellLay = new QHBoxLayout(shell);
    shellLay->setContentsMargins(22, 22, 22, 22);
    shellLay->setSpacing(24);
    root->addWidget(shell, 1);

    auto* centerCol = new QVBoxLayout();
    centerCol->setContentsMargins(0, 12, 0, 0);
    centerCol->setSpacing(14);

    auto* topRow = new QHBoxLayout();
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->addStretch(1);
    auto* workspaceCombo = new QComboBox(shell);
    workspaceCombo->setObjectName(QStringLiteral("welcomeWorkspaceCombo"));
    workspaceCombo->addItem(tr("Workspaces"));
    workspaceCombo->addItem(tr("Edit"));
    workspaceCombo->addItem(tr("Retouch"));
    topRow->addWidget(workspaceCombo);
    centerCol->addLayout(topRow);

    auto* heroLogo = new QLabel(shell);
    heroLogo->setObjectName(QStringLiteral("welcomeHeroLogo"));
    heroLogo->setPixmap(QIcon(QStringLiteral(":/icons/lumen_logo_512.png")).pixmap(112, 112));
    heroLogo->setFixedSize(118, 118);
    heroLogo->setAlignment(Qt::AlignCenter);
    centerCol->addWidget(heroLogo, 0, Qt::AlignHCenter);

    auto* title = makeLabel(tr("<span style='color:#CCFF00'>Lumen</span> Photo Studio"),
                            shell, "welcomeTitle");
    title->setAlignment(Qt::AlignCenter);
    centerCol->addWidget(title);

    auto* subtitle = makeLabel(tr("Precision Light Editing"), shell, "welcomeSubtitle");
    subtitle->setAlignment(Qt::AlignCenter);
    centerCol->addWidget(subtitle);

    m_dropZone = new QFrame(shell);
    m_dropZone->setObjectName(QStringLiteral("welcomeDropZone"));
    m_dropZone->setMinimumHeight(118);
    m_dropZone->setMaximumWidth(640);
    auto* dropLay = new QVBoxLayout(m_dropZone);
    dropLay->setContentsMargins(20, 18, 20, 18);
    dropLay->setSpacing(6);
    auto* uploadIcon = makeLabel(tr("UP"), m_dropZone, "welcomeUploadIcon");
    uploadIcon->setAlignment(Qt::AlignCenter);
    dropLay->addWidget(uploadIcon);
    auto* dropTitle = makeLabel(tr("Drag & drop an image here"), m_dropZone,
                                "welcomeDropTitle");
    dropTitle->setAlignment(Qt::AlignCenter);
    dropLay->addWidget(dropTitle);
    auto* dropHint = makeLabel(tr("PNG, JPG, TIFF, BMP, WEBP, RAW"), m_dropZone,
                               "welcomeDropHint");
    dropHint->setAlignment(Qt::AlignCenter);
    dropLay->addWidget(dropHint);
    centerCol->addWidget(m_dropZone, 0, Qt::AlignHCenter);

    auto* recentCard = new QFrame(shell);
    recentCard->setObjectName(QStringLiteral("welcomeRecentCard"));
    auto* recentLay = new QVBoxLayout(recentCard);
    recentLay->setContentsMargins(22, 18, 22, 18);
    recentLay->setSpacing(14);
    auto* imageTop = new QHBoxLayout();
    imageTop->setContentsMargins(0, 0, 0, 0);
    imageTop->setSpacing(8);
    imageTop->addWidget(makeLabel(tr("Recent Images"), recentCard,
                                  "welcomeRecentTitle"));
    imageTop->addStretch(1);
    auto* viewAll = new QPushButton(tr("View All >"), recentCard);
    viewAll->setObjectName(QStringLiteral("welcomeLinkButton"));
    viewAll->setCursor(Qt::PointingHandCursor);
    imageTop->addWidget(viewAll);
    recentLay->addLayout(imageTop);

    m_recentImagesGrid = new QGridLayout();
    m_recentImagesGrid->setContentsMargins(0, 0, 0, 0);
    m_recentImagesGrid->setHorizontalSpacing(18);
    m_recentImagesGrid->setVerticalSpacing(16);
    recentLay->addLayout(m_recentImagesGrid);

    recentLay->addSpacing(4);
    recentLay->addWidget(makeLabel(tr("Recent Projects"), recentCard,
                                   "welcomeRecentTitle"));

    m_recentProjectsGrid = new QGridLayout();
    m_recentProjectsGrid->setContentsMargins(0, 0, 0, 0);
    m_recentProjectsGrid->setHorizontalSpacing(18);
    m_recentProjectsGrid->setVerticalSpacing(16);
    recentLay->addLayout(m_recentProjectsGrid);
    setRecentItems(QStringList(), QStringList());
    centerCol->addWidget(recentCard, 1);

    m_showOnStartupCheck = new QCheckBox(tr("Show this screen on startup"), shell);
    m_showOnStartupCheck->setObjectName(QStringLiteral("welcomeStartupCheck"));
    m_showOnStartupCheck->setChecked(true);
    m_showOnStartupCheck->setCursor(Qt::PointingHandCursor);
    connect(m_showOnStartupCheck, &QCheckBox::toggled,
            this, &WelcomeScreenWidget::showOnStartupChanged);
    centerCol->addWidget(m_showOnStartupCheck, 0, Qt::AlignHCenter);
    shellLay->addLayout(centerCol, 1);

    // ---- Right panel -------------------------------------------------------
    auto* rightCol = new QVBoxLayout();
    rightCol->setContentsMargins(0, 0, 0, 32);
    rightCol->setSpacing(18);
    rightCol->setSizeConstraint(QLayout::SetMinimumSize);
    rightCol->addStretch(2);

    auto* quickCard = makeInfoCard(shell, tr("Quick Start"));
    auto* quickLay = static_cast<QVBoxLayout*>(quickCard->layout());
    const QStringList steps = {
        tr("Open an image to start editing"),
        tr("Use the tool panels on the right"),
        tr("Adjust, mask, and perfect"),
        tr("Export your masterpiece"),
    };
    for (const QString& step : steps)
        quickLay->addWidget(makeLabel(step, quickCard, "welcomeCardText"));
    rightCol->addWidget(quickCard);

    auto* tipsCard = makeInfoCard(shell, tr("Tips & Tricks"));
    auto* tipsLay = static_cast<QVBoxLayout*>(tipsCard->layout());
    auto* video = new QFrame(tipsCard);
    video->setObjectName(QStringLiteral("welcomeVideoPreview"));
    video->setMinimumHeight(136);
    auto* videoLay = new QVBoxLayout(video);
    videoLay->addStretch(1);
    auto* play = makeLabel(tr(">"), video, "welcomePlayButton");
    play->setAlignment(Qt::AlignCenter);
    videoLay->addWidget(play, 0, Qt::AlignCenter);
    videoLay->addStretch(1);
    tipsLay->addWidget(video);
    tipsLay->addWidget(makeLabel(tr("Getting started with Lumen"), tipsCard,
                                 "welcomeTipTitle"));
    tipsLay->addWidget(makeLabel(tr("A quick 2 min overview"), tipsCard,
                                 "welcomeCardText"));
    auto* dots = new QHBoxLayout();
    dots->setContentsMargins(0, 4, 0, 0);
    dots->setSpacing(7);
    dots->addStretch(1);
    for (int i = 0; i < 7; ++i) {
        auto* dot = new QLabel(i == 0 ? QStringLiteral("*") : QStringLiteral("."), tipsCard);
        dot->setObjectName(i == 0 ? QStringLiteral("welcomeDotActive")
                                  : QStringLiteral("welcomeDot"));
        dots->addWidget(dot);
    }
    dots->addStretch(1);
    tipsLay->addLayout(dots);
    rightCol->addWidget(tipsCard);
    rightCol->addStretch(3);
    shellLay->addLayout(rightCol);

    setStyleSheet(QStringLiteral(R"(
        QWidget#welcomeScreen {
            background: #05070B;
            color: #E7E9EE;
        }
        QFrame#welcomeSidebar,
        QFrame#welcomeMainShell {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                        stop:0 #111722, stop:0.52 #0B111A, stop:1 #080B11);
            border: 1px solid #222B38;
            border-radius: 14px;
        }
        QFrame#welcomeSeparator {
            color: #222B38;
            background: #222B38;
            max-height: 1px;
        }
        QLabel#welcomeSideLogo,
        QLabel#welcomeHeroLogo {
            background: #080B0F;
            border: 1px solid #AFC6E8;
            border-radius: 12px;
        }
        QLabel#welcomeSideTitle {
            color: #F3F6FB;
            font-size: 16px;
        }
        QPushButton#welcomePrimaryButton {
            background: #CCFF00;
            color: #101114;
            border: 1px solid #CCFF00;
            border-radius: 8px;
            font-weight: 700;
            text-align: left;
            padding-left: 16px;
        }
        QPushButton#welcomePrimaryButton:hover {
            background: #D7FF35;
        }
        QPushButton#welcomeActionButton {
            background: rgba(30, 34, 44, 210);
            color: #F3F6FB;
            border: 1px solid #1F2938;
            border-radius: 8px;
            font-weight: 600;
            text-align: left;
            padding-left: 16px;
        }
        QPushButton#welcomeActionButton:hover {
            border-color: #CCFF00;
            color: #CCFF00;
        }
        QLabel#welcomeSectionLabel,
        QLabel#welcomeRecentMeta,
        QLabel#welcomeVersion,
        QLabel#welcomeCardText,
        QLabel#welcomeDropHint {
            color: #9AA3B2;
            font-size: 12px;
        }
        QToolButton#welcomeOtherButton {
            background: transparent;
            border: 1px solid transparent;
            border-radius: 7px;
            color: #E3E8F1;
            text-align: left;
            padding-left: 2px;
        }
        QToolButton#welcomeOtherButton:hover {
            background: rgba(204,255,0,24);
            color: #CCFF00;
        }
        QLabel#welcomeSocialIcon {
            color: #A8B0BE;
            font-size: 20px;
        }
        QComboBox#welcomeWorkspaceCombo {
            background: rgba(18, 24, 34, 210);
            color: #F3F6FB;
            border: 1px solid #263245;
            border-radius: 8px;
            padding: 8px 14px;
            min-width: 130px;
        }
        QLabel#welcomeTitle {
            color: #F8FAFF;
            font-size: 38px;
            font-weight: 800;
        }
        QLabel#welcomeSubtitle {
            color: #D2D8E4;
            font-size: 17px;
        }
        QFrame#welcomeDropZone {
            background: rgba(10, 15, 24, 150);
            border: 1px dashed #526074;
            border-radius: 10px;
        }
        QFrame#welcomeDropZone[dragHover="true"] {
            border: 1px dashed #CCFF00;
            background: rgba(204,255,0,20);
        }
        QLabel#welcomeUploadIcon {
            color: #DCE6F5;
            font-size: 28px;
        }
        QLabel#welcomeDropTitle {
            color: #F5F7FB;
            font-size: 16px;
            font-weight: 700;
        }
        QFrame#welcomeRecentCard,
        QFrame#welcomeInfoCard {
            background: rgba(14, 19, 29, 218);
            border: 1px solid #263245;
            border-radius: 12px;
        }
        QLabel#welcomeRecentTitle,
        QLabel#welcomeCardTitle,
        QLabel#welcomeTipTitle {
            color: #F7F9FC;
            font-weight: 700;
            font-size: 15px;
        }
        QPushButton#welcomeLinkButton {
            background: transparent;
            border: none;
            color: #B28CFF;
            font-weight: 600;
        }
        QFrame#welcomeRecentItem:hover {
            background: rgba(204,255,0,18);
            border-radius: 8px;
        }
        QFrame#welcomeThumb {
            border: 1px solid #304057;
            border-radius: 7px;
        }
        QFrame#welcomeThumb[tone="0"] {
            background: qlineargradient(x1:0,y1:0,x2:1,y2:1,
                        stop:0 #87A9C9, stop:0.45 #273B4E, stop:1 #A7D056);
        }
        QFrame#welcomeThumb[tone="1"] {
            background: qlineargradient(x1:0,y1:0,x2:1,y2:1,
                        stop:0 #D0A96A, stop:0.48 #3A3F32, stop:1 #101722);
        }
        QFrame#welcomeThumb[tone="2"] {
            background: qlineargradient(x1:0,y1:0,x2:1,y2:1,
                        stop:0 #1A2540, stop:0.45 #D43D7D, stop:1 #E98B22);
        }
        QFrame#welcomeThumb[tone="3"] {
            background: qlineargradient(x1:0,y1:0,x2:1,y2:1,
                        stop:0 #FAD9AA, stop:0.45 #4B6D9D, stop:1 #171E2C);
        }
        QFrame#welcomeThumb[tone="4"] {
            background: qlineargradient(x1:0,y1:0,x2:1,y2:1,
                        stop:0 #F3673D, stop:0.42 #101A2B, stop:1 #3FA7D8);
        }
        QLabel#welcomeRawBadge {
            background: rgba(12, 16, 22, 185);
            border: 1px solid #7C8799;
            border-radius: 4px;
            color: #F0F3F7;
            font-size: 9px;
            padding: 2px 4px;
        }
        QLabel#welcomeRecentName {
            color: #F3F6FB;
            font-size: 12px;
        }
        QLabel#welcomeThumbFallback {
            color: #F3F6FB;
            font-size: 18px;
            font-weight: 800;
        }
        QFrame#welcomeVideoPreview {
            background: qlineargradient(x1:0,y1:0,x2:1,y2:1,
                        stop:0 #261C42, stop:0.55 #172132, stop:1 #71512C);
            border: 1px solid #2F3B50;
            border-radius: 8px;
        }
        QLabel#welcomePlayButton {
            color: #FFFFFF;
            background: rgba(255,255,255,42);
            border: 1px solid rgba(255,255,255,90);
            border-radius: 20px;
            min-width: 40px;
            min-height: 40px;
        }
        QLabel#welcomeDotActive {
            color: #CCFF00;
        }
        QLabel#welcomeDot {
            color: #536071;
        }
        QCheckBox#welcomeStartupCheck {
            color: #DDE3EE;
            spacing: 8px;
        }
        QCheckBox#welcomeStartupCheck::indicator {
            width: 16px;
            height: 16px;
            border-radius: 4px;
            border: 1px solid #536071;
            background: #121822;
        }
        QCheckBox#welcomeStartupCheck::indicator:checked {
            background: #CCFF00;
            border-color: #CCFF00;
        }
    )"));
}

bool WelcomeScreenWidget::showOnStartup() const
{
    return !m_showOnStartupCheck || m_showOnStartupCheck->isChecked();
}

void WelcomeScreenWidget::setShowOnStartup(bool on)
{
    if (!m_showOnStartupCheck) return;
    QSignalBlocker block(m_showOnStartupCheck);
    m_showOnStartupCheck->setChecked(on);
}

void WelcomeScreenWidget::setRecentItems(const QStringList& images,
                                         const QStringList& projects)
{
    if (!m_recentImagesGrid || !m_recentProjectsGrid) return;

    auto clearGrid = [](QGridLayout* grid) {
        while (QLayoutItem* item = grid->takeAt(0)) {
            if (QWidget* w = item->widget())
                delete w;
            delete item;
        }
    };
    clearGrid(m_recentImagesGrid);
    clearGrid(m_recentProjectsGrid);

    struct RecentItem { QString name; QString time; QString tag; QString path; };
    QVector<RecentItem> imageItems;
    QVector<RecentItem> projectItems;
    imageItems.reserve(5);
    projectItems.reserve(5);

    auto appendPath = [this](QVector<RecentItem>& items,
                             const QString& path,
                             const QString& tag) {
        if (items.size() >= 5) return;
        const QFileInfo fi(path);
        if (!fi.isFile()) return;
        const QString fileName = fi.fileName();
        const QString when = fi.lastModified().isValid()
            ? fi.lastModified().toString(QStringLiteral("M/d/yyyy h:mm AP"))
            : tr("Recent");
        items.push_back(RecentItem{ fileName, when, tag, fi.absoluteFilePath() });
    };

    static const QStringList rawExts = {
        QStringLiteral("cr2"), QStringLiteral("cr3"),
        QStringLiteral("nef"), QStringLiteral("arw"),
        QStringLiteral("dng"), QStringLiteral("raf"),
        QStringLiteral("orf"), QStringLiteral("rw2"),
    };
    for (const QString& path : images) {
        const QString ext = QFileInfo(path).suffix().toLower();
        const bool raw = rawExts.contains(ext);
        appendPath(imageItems, path, raw ? tr("RAW") : QString());
    }
    for (const QString& path : projects)
        appendPath(projectItems, path, tr("PROJECT"));

    if (imageItems.isEmpty()) {
        imageItems.push_back(RecentItem{
            tr("No recent images"),
            tr("Open an image to begin"),
            QString(),
            QString(),
        });
    }
    if (projectItems.isEmpty()) {
        projectItems.push_back(RecentItem{
            tr("No recent projects"),
            tr("Save or open a project"),
            QString(),
            QString(),
        });
    }

    auto populateGrid = [this](QGridLayout* grid,
                               const QVector<RecentItem>& items,
                               bool projectGrid) {
        for (int i = 0; i < items.size() && i < 5; ++i) {
            const RecentItem& item = items[i];
            std::function<void()> activated;
            if (!item.path.isEmpty()) {
                const QString path = item.path;
                activated = [this, path, projectGrid]() {
                    if (projectGrid)
                        emit recentProjectRequested(path);
                    else
                        emit recentImageRequested(path);
                };
            }
            grid->addWidget(makeRecentItem(this,
                                           item.name,
                                           item.time,
                                           item.tag,
                                           i,
                                           item.path,
                                           std::move(activated)),
                            0, i);
        }
    };

    populateGrid(m_recentImagesGrid, imageItems, false);
    populateGrid(m_recentProjectsGrid, projectItems, true);
}

void WelcomeScreenWidget::setRecentFiles(const QStringList& files,
                                         const QStringList& projects)
{
    setRecentItems(files, projects);
}

bool WelcomeScreenWidget::eventHasSupportedImageFile(const QMimeData* mime,
                                                     QString* outPath)
{
    if (!mime || !mime->hasUrls()) return false;

    const QList<QUrl> urls = mime->urls();
    if (urls.size() != 1) return false;

    const QUrl& url = urls.first();
    if (!url.isLocalFile()) return false;

    const QString path = url.toLocalFile();
    const QFileInfo fi(path);
    if (!fi.isFile()) return false;
    if (!supportedExts().contains(fi.suffix().toLower())) return false;

    if (outPath) *outPath = path;
    return true;
}

void WelcomeScreenWidget::setDropHovering(bool hovering)
{
    if (m_dropHovering == hovering) return;
    m_dropHovering = hovering;
    if (!m_dropZone) return;

    m_dropZone->setProperty("dragHover", hovering);
    m_dropZone->style()->unpolish(m_dropZone);
    m_dropZone->style()->polish(m_dropZone);
    m_dropZone->update();
}

void WelcomeScreenWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (eventHasSupportedImageFile(event->mimeData())) {
        setDropHovering(true);
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

void WelcomeScreenWidget::dragMoveEvent(QDragMoveEvent* event)
{
    if (eventHasSupportedImageFile(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

void WelcomeScreenWidget::dragLeaveEvent(QDragLeaveEvent* event)
{
    setDropHovering(false);
    event->accept();
}

void WelcomeScreenWidget::dropEvent(QDropEvent* event)
{
    QString path;
    if (eventHasSupportedImageFile(event->mimeData(), &path)) {
        setDropHovering(false);
        emit imageFileDropped(path);
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}
