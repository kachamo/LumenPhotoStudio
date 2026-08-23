// ==============================================================================
// catalog/CatalogTypes.h
// Plain data types shared by the catalog layer and its UI.
//
// The catalog is the Library half of the application: it tracks every image in
// the folders the user has added, independently of which one is open in the
// editor. Editing state lives in `Look` (see core/Look.h); the catalog stores a
// serialized Look per image so edits survive without touching the original file.
// ==============================================================================
#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

namespace lps {

// Flag follows the Lightroom pick/reject convention.
enum class ImageFlag : int { Rejected = -1, None = 0, Picked = 1 };

// 0 = unlabelled. The names are conventional; the UI maps them to colours.
enum class ColorLabel : int { None = 0, Red = 1, Yellow = 2, Green = 3, Blue = 4, Purple = 5 };

enum class SortKey : int { CaptureTime, FileName, Rating, ImportedAt, FileSize };

// One row of the `images` table, denormalized for display.
struct CatalogImage
{
    qint64    id           = -1;
    qint64    folderId     = -1;
    QString   absolutePath;          // resolved: folder.path + relativePath
    QString   fileName;
    QString   relativePath;          // relative to the folder root
    qint64    fileSize     = 0;
    QDateTime fileModified;
    int       width        = 0;
    int       height       = 0;
    bool      isRaw        = false;

    QDateTime captureTime;           // from EXIF; invalid if unknown
    QString   cameraModel;
    QString   lensModel;
    QString   iso;
    QString   aperture;
    QString   shutterSpeed;
    QString   focalLength;

    int        rating     = 0;                    // 0..5
    ImageFlag  flag       = ImageFlag::None;
    ColorLabel colorLabel = ColorLabel::None;

    QStringList keywords;
    QString     lookJson;            // serialized Look, empty = unedited
    QDateTime   importedAt;

    bool isValid() const { return id >= 0; }
};

struct CatalogFolder
{
    qint64    id = -1;
    QString   path;
    QDateTime addedAt;
    int       imageCount = 0;        // populated by queries that ask for it
};

// Query filter. Default-constructed means "everything, newest capture first".
struct CatalogFilter
{
    qint64      folderId    = -1;           // -1 = all folders
    int         minRating   = 0;            // 0 = no rating filter
    bool        pickedOnly  = false;
    bool        hideRejected = true;
    ColorLabel  colorLabel  = ColorLabel::None;   // None = any
    QString     searchText;                 // matches filename / camera / lens / keyword
    QStringList keywords;                   // AND-ed
    SortKey     sortKey     = SortKey::CaptureTime;
    bool        ascending   = false;

    bool isDefault() const;
};

struct CatalogStats
{
    int folderCount = 0;
    int imageCount  = 0;
    int rawCount    = 0;
    int pickedCount = 0;
    int rejectedCount = 0;
};

} // namespace lps
