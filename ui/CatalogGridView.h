// ==============================================================================
// ui/CatalogGridView.h
// Thumbnail grid for the Library.
//
// Must stay smooth with tens of thousands of images: thumbnails load lazily,
// off the UI thread, and only for tiles actually on screen.
// ==============================================================================
#pragma once

#include "catalog/CatalogTypes.h"

#include <QVector>
#include <QWidget>

class CatalogGridView : public QWidget
{
    Q_OBJECT

public:
    explicit CatalogGridView(QWidget* parent = nullptr);
    ~CatalogGridView() override;

    void setImages(const QVector<lps::CatalogImage>& images);
    void clear();

    QVector<qint64> selectedIds() const;
    int             thumbnailSize() const;

public slots:
    void setThumbnailSize(int px);

    // Additive to the original contract: refresh a single row in place after a
    // catalog write, without the model reset that setImages() would cause.
    void updateImage(const lps::CatalogImage& image);

signals:
    void imageActivated(const QString& absolutePath);   // double-click / Enter
    void selectionChanged(const QVector<qint64>& imageIds);
    void ratingRequested(qint64 imageId, int rating);
    void flagRequested(qint64 imageId, lps::ImageFlag flag);

private:
    struct Impl;
    Impl* d;
};
