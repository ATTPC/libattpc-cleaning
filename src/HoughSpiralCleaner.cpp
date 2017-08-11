//
// Created by Joshua Bradt on 7/28/17.
//

#include "HoughSpiralCleaner.h"

namespace {
    template <class Derived>
    inline Eigen::Array<typename Derived::Scalar, Derived::RowsAtCompileTime, Derived::ColsAtCompileTime>
    houghLineFunc(const Eigen::ArrayBase<Derived>& x, const double rad, const double theta) {
        return (rad - x * std::cos(theta)) / std::sin(theta);
    }
}

namespace attpc {
namespace cleaning {

HoughSpiralCleaner::HoughSpiralCleaner(const HoughSpiralCleanerConfig& config)
: numAngleBinsToReduce(config.numAngleBinsToReduce)
, houghSpaceSliceSize(config.houghSpaceSliceSize)
, peakWidth(config.peakWidth)
, minPointsPerLine(config.minPointsPerLine)
, linHough(config.linearHoughNumBins, config.linearHoughMaxRadius)
, circHough(config.circularHoughNumBins, config.circularHoughMaxRadius)
{}

Eigen::ArrayXd HoughSpiralCleaner::findArcLength(const Eigen::ArrayXXd& xy, const Eigen::Vector2d center) const {
    const Eigen::ArrayXd xOffset = xy.col(0) - center(0);
    const Eigen::ArrayXd yOffset = xy.col(1) - center(1);

    Eigen::ArrayXd rads = Eigen::sqrt(xOffset.square() + yOffset.square());
    Eigen::ArrayXd thetas = Eigen::atan(yOffset / xOffset);

    return rads * thetas;
}

HoughSpace HoughSpiralCleaner::findHoughSpace(const Eigen::ArrayXd& zs, const Eigen::ArrayXd& arclens) const {
    assert(zs.rows() == arclens.rows());

    Eigen::ArrayXXd houghData {zs.rows(), 2};  // XXX: This copy might be unnecessary if the code were written better
    houghData.col(0) = zs;
    houghData.col(1) = arclens;

    return linHough.findHoughSpace(houghData);
}

Eigen::Index HoughSpiralCleaner::findMaxAngleBin(const HoughSpace& houghSpace) const {
    // Find the ordering of indices that would sort the array
    using coefArrayType = Eigen::Array<Eigen::Index, 2, 1>;
    std::vector<coefArrayType> indices;
    for (Eigen::Index i = 0; i < houghSpace.getNumBins(); ++i) {
        for (Eigen::Index j = 0; j < houghSpace.getNumBins(); ++j) {
            indices.emplace_back(i, j);
        }
    }

    std::sort(indices.begin(), indices.end(), [&houghSpace](auto idxA, auto idxB) {
        return houghSpace.getValueAtBin(idxA(0), idxA(1)) < houghSpace.getValueAtBin(idxB(0), idxB(1));
    });

    // Find the mean bin from the last few elements of the sorted list
    // The zero element must be declared separately for std::accumulate to work
    const coefArrayType zeroBin = coefArrayType::Zero(2, 1);
    coefArrayType binTotal = std::accumulate(indices.end() - numAngleBinsToReduce, indices.end(), zeroBin);
    coefArrayType meanBin = Eigen::floor(binTotal / numAngleBinsToReduce);

    return meanBin(0);
}

auto HoughSpiralCleaner::findMaxAngleSlice(const HoughSpace& houghSpace, const Eigen::Index maxAngleBin) const -> AngleSliceArrayType {
    HoughSpace::ConstDataBlockType block = houghSpace.getAngularSlice(maxAngleBin - houghSpaceSliceSize, 2 * houghSpaceSliceSize);

    // HACK: Make sure the shape of the chunk is what's expected. This might not be the case if the order of the
    // dimensions was changed in the internal matrix of the HoughSpace class. This should probably be done in a
    // less ugly way.
    assert((block.rows() == 2 * houghSpaceSliceSize) && (block.cols() == houghSpace.getNumBins()));

    return block.colwise().sum();
}

std::vector<double> HoughSpiralCleaner::findPeakRadiusBins(const AngleSliceArrayType& houghSlice) const {
    const std::vector<Eigen::Index> maxLocs = findPeakLocations(houghSlice, 2);
    std::vector<double> peakCtrs;

    for (const Eigen::Index pkIdx : maxLocs) {
        const Eigen::Index firstPt = std::max(pkIdx - peakWidth, Eigen::Index{0});
        const Eigen::Index lastPt = std::max(pkIdx + peakWidth, houghSlice.rows() - 1);

        const Eigen::VectorXd positions = Eigen::VectorXd::LinSpaced(lastPt - firstPt + 1, firstPt, lastPt);
        const Eigen::VectorXd values = houghSlice.segment(firstPt, lastPt - firstPt + 1).cast<double>();
        const double peakCtrOfGrav = positions.dot(values) / values.sum();

        peakCtrs.push_back(peakCtrOfGrav);
    }

    return peakCtrs;
}

HoughSpiralCleanerResult::HoughSpiralCleanerResult(const Eigen::Index numPts)
: labels(decltype(labels)::Constant(numPts, -1))
, distancesToNearestLine(decltype(distancesToNearestLine)::Constant(numPts, std::numeric_limits<double>::infinity()))
{}

HoughSpiralCleanerResult HoughSpiralCleaner::classifyPoints(
        const Eigen::ArrayXXd& xyz, const Eigen::ArrayXd& arclens,
        const double maxAngle, const Eigen::ArrayXd& radii) const {

    const Eigen::Index numLinesFound = radii.rows();  // Each rad peak is a found line
    const Eigen::Index numPts = xyz.rows();

    HoughSpiralCleanerResult result {numPts};
    Eigen::Array<Eigen::Index, Eigen::Dynamic, 1> pointsPerLine = decltype(pointsPerLine)::Zero(numLinesFound);

    for (Eigen::Index lineIdx = 0; lineIdx < numLinesFound; ++lineIdx) {
        const double rad = radii(lineIdx);
        const Eigen::ArrayXd dist = Eigen::abs(houghLineFunc(xyz.col(2), rad, maxAngle) - arclens);

        // Iterate over each point and set its result to the current line if the
        // current line is better than the previous one.
        for (Eigen::Index pointIdx = 0; pointIdx < numPts; ++pointIdx) {
            const auto newDist = dist(pointIdx);
            const auto oldDist = result.distancesToNearestLine(pointIdx);
            if (newDist < oldDist) {
                // Update point counts before updating results
                const Eigen::Index oldLineIdx = result.labels(pointIdx);
                --pointsPerLine(oldLineIdx);
                ++pointsPerLine(lineIdx);

                result.labels(pointIdx) = lineIdx;
                result.distancesToNearestLine(pointIdx) = newDist;
            }
        }
    }

    // Eliminate lines that have too few points
    for (Eigen::Index lineIdx = 0; lineIdx < numLinesFound; ++lineIdx) {
        if (pointsPerLine(lineIdx) < minPointsPerLine) {
            for (Eigen::Index pointIdx = 0; pointIdx < numPts; ++pointIdx) {
                if (result.labels(pointIdx) == lineIdx) {
                    result.labels(pointIdx) = -1;
                    result.distancesToNearestLine = std::numeric_limits<double>::infinity();
                }
            }
        }
    }

    return result;
}

}
}
