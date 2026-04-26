// ==============================================================================
// ui/HistogramWidget.h
// Live histogram panel for the edited preview image.
//
// Displays four 256-bin distributions: red, green, blue, and Rec.709
// luminance. R/G/B are drawn with additive blending so overlap regions
// show their additive sum (Lightroom/Photoshop convention); luminance is
// a faint white overlay on top.
//
// Bins are computed once per setImage() call and cached. paintEvent reads
// from the cache only — repaints during widget resize are free.
//
// Sampling is subsampled for large images: at most ~kMaxSamples pixels
// contribute to the bins. With 256 bins and ~250K samples that's ~1000
// pixels per bin on a flat image — plenty of statistical signal.
// ==============================================================================
#pragma once

#include <QImage>
#include <QWidget>

#include <array>

class HistogramWidget : public QWidget
{
    Q_OBJECT

public:
    explicit HistogramWidget(QWidget* parent = nullptr);

    // Recompute bins from the given image. Subsamples for large images.
    // Calling with a null image clears the histogram (paint shows just the
    // empty grid + frame).
    void setImage(const QImage& image);

    QSize sizeHint()        const override { return QSize(280, 110); }
    QSize minimumSizeHint() const override { return QSize(180,  72); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    static constexpr int kBinCount   = 256;
    static constexpr int kMaxSamples = 250000;   // soft cap on samples per setImage

    // Per-channel bin storage. Each entry counts pixels whose channel value
    // falls in [bin/255, (bin+1)/255). Filled by setImage; read by paintEvent.
    std::array<int, kBinCount> m_binsR{};
    std::array<int, kBinCount> m_binsG{};
    std::array<int, kBinCount> m_binsB{};
    std::array<int, kBinCount> m_binsY{};

    // Robust peak — used as the y-axis ceiling. We use the 99th percentile
    // bin height instead of the absolute max so a single saturated bin (e.g.
    // a flat sky dumping pixels into a tiny range) doesn't visually flatten
    // the rest of the histogram.
    int m_peak = 0;

    // True if a meaningful number of pixels are clipped at the dark or
    // bright endpoint in any channel. Drives the small triangle indicators
    // at the histogram edges.
    bool m_clippedShadows    = false;
    bool m_clippedHighlights = false;

    // Total samples actually counted (after subsampling). Used to compute
    // the clipping percentages and the peak threshold.
    int m_sampleCount = 0;
};
