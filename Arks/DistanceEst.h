#ifndef _DISTANCE_EST_H_
#define _DISTANCE_EST_H_ 1

#include "Arks/Arks.h"
#include "Common/MapUtil.h"
#include "Common/PairHash.h"
#include "Common/StatUtil.h"
#include <cstdlib>
#include <limits>
#include <iostream>
#include <utility>

/** min/max distance estimate for a pair contigs */
struct DistanceEstimate
{
	int minDist;
	int maxDist;
	double jaccard;

	DistanceEstimate() : minDist(0), maxDist(0), jaccard(0.0) {}
};

/**
 * Records the distance between the head/tail regions of the same
 * contig vs. barcode union size, barcode intersection size,
 * and number of distinct barcodes mapped to each end.
 */
struct DistSample
{
	unsigned distance;
	unsigned barcodesHead;
	unsigned barcodesTail;
	unsigned barcodesUnion;
	unsigned barcodesIntersect;

	DistSample() :
		distance(std::numeric_limits<unsigned>::max()),
		barcodesHead(0),
		barcodesTail(0),
		barcodesUnion(0),
		barcodesIntersect(0)
	{}
};

typedef std::unordered_map<std::string, DistSample> DistSampleMap;
typedef typename DistSampleMap::const_iterator DistSampleConstIt;

typedef std::map<double, DistSample> JaccardToDist;
typedef typename JaccardToDist::const_iterator JaccardToDistConstIt;

/**
 * Measure distance between contig ends vs.
 * barcode intersection size and barcode union size.
 */
void calcDistSamples(const ARCS::IndexMap& imap,
	const ARCS::ContigToLength& contigToLength,
	const std::unordered_map<std::string, int>& indexMultMap,
	const ARCS::ArcsParams& params,
	DistSampleMap& distSamples)
{
	/* for each chromium barcode */
	for (auto barcodeIt = imap.begin(); barcodeIt != imap.end();
		++barcodeIt)
	{
		/* skip barcodes outside of min/max multiplicity range */
		std::string index = barcodeIt->first;
		int indexMult = indexMultMap.at(index);
		if (indexMult < params.min_mult || indexMult > params.max_mult)
			continue;

		/* contig head/tail => number of mapped read pairs */
		const ARCS::ScafMap& contigToCount = barcodeIt->second;

		for (auto contigIt = contigToCount.begin();
			contigIt != contigToCount.end(); ++contigIt)
		{
			std::string contigID;
			bool isHead;
			std::tie(contigID, isHead) = contigIt->first;
			int readPairs = contigIt->second;

			/*
			 * skip contigs with less than required number of
			 * mapped read pairs (-c option)
			 */
			if (readPairs < params.min_reads)
				continue;

			/*
			 * skip contigs shorter than 2 times the contig
			 * end length, because we want our distance samples
			 * to be based on a uniform head/tail length
			 */

			unsigned l = contigToLength.at(contigID);
			if (l < (unsigned) 2 * params.end_length)
				continue;

			DistSample& distSample = distSamples[contigID];
			distSample.distance = l - 2 * params.end_length;

			if (isHead)
				distSample.barcodesHead++;
			else
				distSample.barcodesTail++;

			/*
			 * Check if barcode also maps to other end of contig
			 * with sufficient number of read pairs.
			 *
			 * The `isHead` part of the `if` condition prevents
			 * double-counting when a barcode maps to both
			 * ends of a contig.
			 */

			ARCS::CI otherEnd(contigID, !isHead);
			ARCS::ScafMapConstIt otherIt = contigToCount.find(otherEnd);
			bool foundOther = otherIt != contigToCount.end()
				&& otherIt->second >= params.min_reads;

			if (foundOther && isHead) {
				distSample.barcodesIntersect++;
				distSample.barcodesUnion++;
			} else if (!foundOther) {
				distSample.barcodesUnion++;
			}
		}
	}
}

/**
 * Build a ordered map from barcode Jaccard index to
 * distance sample. Each distance sample comes from
 * measuring the distance between the head/tail of the
 * same contig, along with associated head/tail barcode
 * counts.
 */
static inline void buildJaccardToDist(
	const DistSampleMap& distSamples,
	JaccardToDist& jaccardToDist)
{
	for (DistSampleConstIt it = distSamples.begin();
		it != distSamples.end(); ++it)
	{
		const DistSample& sample = it->second;
		double jaccard = double(sample.barcodesIntersect)
			/ sample.barcodesUnion;
		jaccardToDist.insert(
			JaccardToDist::value_type(jaccard, sample));
	}
}

/**
 * Check requirements for using the given barcode-to-contig-end mapping
 * in distance estimates. Return true if we should the given
 * mapping in our calculations.
 */
static inline bool validBarcodeMapping(unsigned contigLength,
	int pairs, const ARCS::ArcsParams& params)
{
	/*
	 * skip contigs with less than required number of
	 * mapped read pairs (-c option)
	 */

	if (pairs < params.min_reads)
		return false;

	/*
	 * skip contigs shorter than 2 times the contig
	 * end length, our distance samples are based
	 * based on a uniform head/tail length
	 */

	if (contigLength < unsigned(2 * params.end_length))
		return false;

	return true;
}

/** calculate shared barcode stats for candidate contig pairs */
static inline void calcContigPairBarcodeStats(
	const ARCS::IndexMap& imap,
	const std::unordered_map<std::string, int>& indexMultMap,
	const ARCS::ContigToLength& contigToLength,
	const ARCS::ArcsParams& params,
	ARCS::PairMap& pmap)
{
	typedef std::unordered_map<ARCS::CI, size_t, PairHash> ContigEndToBarcodeCount;
	ContigEndToBarcodeCount contigEndToBarcodeCount;

	/* calculate number of shared barcodes for candidate contig end pairs */

	for (auto barcodeIt = imap.begin(); barcodeIt != imap.end();
		++barcodeIt)
	{
		/* skip barcodes outside of min/max multiplicity range */
		std::string index = barcodeIt->first;
		int indexMult = indexMultMap.at(index);
		if (indexMult < params.min_mult || indexMult > params.max_mult)
			continue;

		/* contig head/tail => number of mapped read pairs */
		const ARCS::ScafMap& contigEndToPairCount = barcodeIt->second;

		for (auto endIt1 = contigEndToPairCount.begin();
			endIt1 != contigEndToPairCount.end(); ++endIt1)
		{
			/* get contig ID and head/tail flag */
			std::string id1;
			bool head1;
			std::tie(id1, head1) = endIt1->first;

			/* check requirements for calculating distance estimates */
			unsigned length1 = contigToLength.at(id1);
			int pairs1 = endIt1->second;
			if (!validBarcodeMapping(length1, pairs1, params))
				continue;

			/* count distinct barcodes mapped to head/tail of each contig */
			contigEndToBarcodeCount[endIt1->first]++;

			for (auto endIt2 = contigEndToPairCount.begin();
				 endIt2 != contigEndToPairCount.end(); ++endIt2)
			{
				/* get contig ID and head/tail flag */
				std::string id2;
				bool head2;
				std::tie(id2, head2) = endIt2->first;

				/* check requirements for calculating distance estimates */
				unsigned length2 = contigToLength.at(endIt2->first.first);
				int pairs2 = endIt2->second;
				if (!validBarcodeMapping(length2, pairs2, params))
					continue;

				/* avoid double-counting contig end pairs */
				if (id1 > id2)
					continue;

				/* initialize barcode/weight data for contig end pair */
				ARCS::ContigPair pair(id1, id2);
				if (pmap.count(pair) == 0)
					pmap[pair].fill(ARCS::PairRecord());

				// Head - Head
				if (head1 && head2) {
					pmap[pair][0].barcodesIntersect++;
				// Head - Tail
				} else if (head1 && !head2) {
					pmap[pair][1].barcodesIntersect++;
				// Tail - Head
				} else if (!head1 && head2) {
					pmap[pair][2].barcodesIntersect++;
				// Tail - Tail
				} else if (!head1 && !head2) {
					pmap[pair][3].barcodesIntersect++;
				}
			}
		}
	}

	/*
	 * Compute/store further barcode stats for each candidate
	 * contig pair:
	 *
	 * (1) number of distinct barcodes mapping to contig A (|A|)
	 * (2) number of distinct barcodes mapping to contig B (|B|)
	 * (3) barcode union size for contigs A and B (|A union B|)
	 */

	for (ARCS::PairMapIt it = pmap.begin(); it != pmap.end(); ++it)
	{
		for (ARCS::PairOrientation i = ARCS::HH; i < ARCS::NUM_ORIENTATIONS;
			i = ARCS::PairOrientation(i + 1))
		{
			ARCS::PairRecord& rec = it->second.at(i);

			const std::string& id1 = it->first.first;
			const std::string& id2 = it->first.second;

			ARCS::CI tail1(id1, i == ARCS::HH || i == ARCS::HT);
			ARCS::CI tail2(id2, i == ARCS::HH || i == ARCS::TH);

			rec.barcodes1 = contigEndToBarcodeCount.at(tail1);
			assert(rec.barcodes1 > 0);
			rec.barcodes2 = contigEndToBarcodeCount.at(tail2);
			assert(rec.barcodes2 > 0);

			assert(rec.barcodes1 + rec.barcodes2 >= rec.barcodesIntersect);
			rec.barcodesUnion = rec.barcodes1 + rec.barcodes2
				- rec.barcodesIntersect;
		}
	}
}

/** estimate min/max distance between a pair of contigs */
std::pair<DistanceEstimate, bool> estimateDistance(
	const ARCS::PairRecord& rec, const JaccardToDist& jaccardToDist,
	const ARCS::ArcsParams& params)
{
	DistanceEstimate result;

	/*
	 * if distance estimation was not enabled (`-D`) or input contigs
	 * were too short to provide any training data
	 */

	if (jaccardToDist.empty())
		return std::make_pair(result, false);

	/*
	 * barcodesUnion == 0 when a pair doesn't
	 * meet the requirements for distance estimation,
	 * (e.g. contig length < 2 * params.end_length)
	 */

	if (rec.barcodesUnion == 0)
		return std::make_pair(result, false);

	/* calc jaccard score for current contig pair */

	result.jaccard = double(rec.barcodesIntersect) / rec.barcodesUnion;
	assert(result.jaccard >= 0.0 && result.jaccard <= 1.0);

	/*
	 * get intra-contig distance samples with
	 * with closest Jaccard scores
	 */

	JaccardToDistConstIt lowerIt, upperIt;
	std::tie(lowerIt, upperIt) =
		closestKeys(jaccardToDist, result.jaccard,
			params.dist_bin_size);

	std::vector<unsigned> distances;
	for (JaccardToDistConstIt sampleIt = lowerIt;
		 sampleIt != upperIt; ++sampleIt)
	{
		distances.push_back(sampleIt->second.distance);
	}

	std::sort(distances.begin(), distances.end());

	/* use 99th percentile as upper bound on distance */

	result.minDist =
		(int)floor(quantile(distances.begin(), distances.end(), 0.01));
	result.maxDist =
		(int)ceil(quantile(distances.begin(), distances.end(), 0.99));

	return std::make_pair(result, true);
}

/**
 * Write distance samples to an output stream.  The distance
 * samples record the distance between the head and tail regions
 * of the same contig with associated barcode stats (e.g.
 * barcode intersection size).
 */
static inline std::ostream& writeDistSamples(std::ostream& out,
	const DistSampleMap& distSamples)
{
	out << "contig_id" << '\t'
		<< "distance" << '\t'
		<< "barcodes_head" << '\t'
		<< "barcodes_tail" << '\t'
		<< "barcodes_union" << '\t'
		<< "barcodes_intersect" << '\n';

	for (DistSampleConstIt it = distSamples.begin();
		it != distSamples.end(); ++it)
	{
		const std::string& contigID = it->first;
		const DistSample& sample = it->second;

		out << contigID << '\t'
			<< sample.distance << '\t'
			<< sample.barcodesHead << '\t'
			<< sample.barcodesTail << '\t'
			<< sample.barcodesUnion << '\t'
			<< sample.barcodesIntersect << '\n';
	}

	return out;
}

#endif