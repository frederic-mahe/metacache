/******************************************************************************
 *
 * MetaCache - Meta-Genomic Classification Tool
 *
 * Copyright (C) 2016-2020 André Müller (muellan@uni-mainz.de)
 *                       & Robin Kobus  (kobus@uni-mainz.de)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *****************************************************************************/

#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <iterator>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <iterator>

#include "alignment.h"
#include "candidates.h"
#include "cmdline_utility.h"
#include "options.h"
#include "dna_encoding.h"
#include "matches_per_target.h"
#include "printing.h"
#include "querying.h"
#include "sequence_io.h"
#include "sequence_view.h"
#include "database.h"

#include "classification.h"


namespace mc {

using std::vector;
using std::string;
using std::cout;
using std::cerr;
using std::endl;
using std::flush;


/*************************************************************************//**
 *
 * @brief makes non-owning view to (sub)sequence
 *
 *****************************************************************************/
template<class Sequence>
inline auto
make_view_from_window_range(const Sequence& s, const window_range& range,
                            int size, int stride)
{
    auto end = s.begin() + (stride * range.end) + size;
    if(end > s.end()) end = s.end();

    return make_view(s.begin() + (stride * range.beg), end);
}



/*************************************************************************//**
 *
 * @brief performs a semi-global alignment
 *
 *****************************************************************************/
template<class Subject>
alignment<default_alignment_scheme::score_type,typename sequence::value_type>
make_semi_global_alignment(const sequence_query& query,
                           const Subject& subject)
{
    std::size_t score  = 0;
    std::size_t scorer = 0;

    const auto scheme = default_alignment_scheme{};

    //compute alignment
    auto align = align_semi_global(query.seq1, subject, scheme);
    score = align.score;
    //reverse complement
    auto query1r = make_reverse_complement(query.seq1);
    auto alignr = align_semi_global(query1r, subject, scheme);
    scorer = alignr.score;

    //align paired read as well
    if(!query.seq2.empty()) {
        score += align_semi_global_score(query.seq2, subject, scheme);
        auto query2r = make_reverse_complement(query.seq2);
        scorer += align_semi_global_score(query2r, subject, scheme);
    }

    return (score > scorer) ? align : alignr;
}


/*************************************************************************//**
 *
 * @brief returns query taxon (ground truth for precision tests)
 *
 *****************************************************************************/
const taxon*
ground_truth(const database& db, const string& header)
{
    //try to extract query id and find the corresponding target in database
    const taxon* tax = nullptr;
    tax = db.taxon_with_name(extract_accession_string(header, sequence_id_type::acc_ver));
    if(tax) return db.next_ranked_ancestor(tax);

    tax = db.taxon_with_similar_name(extract_accession_string(header, sequence_id_type::acc));
    if(tax) return db.next_ranked_ancestor(tax);

    //try to extract id from header
    tax = db.taxon_with_id(extract_taxon_id(header));
    if(tax) return db.next_ranked_ancestor(tax);

    //try to find entire header as sequence identifier
    tax = db.taxon_with_name(header);
    if(tax) return db.next_ranked_ancestor(tax);

    return nullptr;
}



/*************************************************************************//**
 *
 * @brief classification cadidates + derived best classification
 *
 *****************************************************************************/
struct classification
{
    classification(classification_candidates cand):
        candidates{std::move(cand)}, best{nullptr}, groundTruth{nullptr}
    {}

    classification_candidates candidates;
    const taxon* best;
    const taxon* groundTruth;
};



/*************************************************************************//**
 *
 * @brief generate classification candidates
 *
 *****************************************************************************/
template<class Locations>
classification_candidates
make_classification_candidates(const database& db,
                               const classification_options& opt,
                               const sequence_query& query,
                               const Locations& allhits)
{
    candidate_generation_rules rules;

    rules.maxWindowsInRange = window_id( 2 + (
        std::max(query.seq1.size() + query.seq2.size(), opt.insertSizeMax) /
        db.target_sketcher().window_stride() ));

    rules.mergeBelow    = opt.lowestRank;
    rules.maxCandidates = opt.maxNumCandidatesPerQuery;

    return classification_candidates{db, allhits, rules};

}



/*************************************************************************//**
 *
 * @brief  classify using top matches/candidates
 *
 *****************************************************************************/
const taxon*
classify(const database& db, const classification_options& opt,
         const classification_candidates& cand)
{
    if(cand.empty() || !cand[0].tax) return nullptr;

    //hits below threshold => considered not classifiable
    if(cand[0].hits < opt.hitsMin) return nullptr;

    // begin lca with first candidate
    const taxon* lca = cand[0].tax;

    // include any candidate in classification that is above threshold
    const float threshold = cand[0].hits > opt.hitsMin
                          ? (cand[0].hits - opt.hitsMin) * opt.hitsDiffFraction
                          : 0;
    // check 2nd, 3rd, ...
    for(auto i = cand.begin()+1; i != cand.end(); ++i) {
        // include all candidates with hits above threshold
        if(i->hits > threshold) {
            // include candidate in lca
            // lca lives on lineage of first cand, its rank can only increase
            lca = db.ranked_lca(cand[0].tgt, i->tgt, lca->rank());
            // exit early if lca rank already too high
            if(!lca || lca->rank() > opt.highestRank)
                return nullptr;
        } else {
            break;
        }
    }
    return (lca->rank() <= opt.highestRank) ? lca : nullptr;
}



/*************************************************************************//**
 *
 * @brief classify using all database matches
 *
 *****************************************************************************/
template<class Locations>
classification
classify(const database& db,
         const classification_options& opt,
         const sequence_query& query,
         const Locations& allhits)
{
    classification cls { make_classification_candidates(db, opt, query, allhits) };

    cls.best = classify(db, opt, cls.candidates);

    return cls;
}



/*************************************************************************//**
 *
 * @brief classify using only matches in tgtMatches
 *
 *****************************************************************************/
void
update_classification(const database& db,
                      const classification_options& opt,
                      classification& cls,
                      const matches_per_target& tgtMatches)
{
    for(auto it = cls.candidates.begin(); it != cls.candidates.end();) {
        if(tgtMatches.find(it->tgt) == tgtMatches.end())
            it = cls.candidates.erase(it);
        else
            ++it;
    }
    cls.best = classify(db, opt, cls.candidates);
}



/*************************************************************************//**
 *
 * @brief add difference between result and truth to statistics
 *
 *****************************************************************************/
void update_coverage_statistics(const database& db,
                                const classification& cls,
                                classification_statistics& stats)
{
    if(!cls.groundTruth) return;
    //check if taxa are covered in DB
    for(const taxon* tax : db.ranks(cls.groundTruth)) {
        if(tax) {
            auto r = tax->rank();
            if(db.covers(*tax)) {
                if(!cls.best || r < cls.best->rank()) { //unclassified on rank
                    stats.count_coverage_false_neg(r);
                } else { //classified on rank
                    stats.count_coverage_true_pos(r);
                }
            }
            else {
                if(!cls.best || r < cls.best->rank()) { //unclassified on rank
                    stats.count_coverage_true_neg(r);
                } else { //classified on rank
                    stats.count_coverage_false_pos(r);
                }
            }
        }
    }
}



/*************************************************************************//**
 *
 * @brief evaluate classification of one query
 *
 *****************************************************************************/
void evaluate_classification(
    const database& db,
    const classification_evaluation_options& opt,
    const sequence_query& query,
    classification& cls,
    classification_statistics& statistics)
{
    if(opt.precision || opt.determineGroundTruth) {
        cls.groundTruth = ground_truth(db, query.header);
    }

    if(opt.precision) {
        auto lca = db.ranked_lca(cls.best, cls.groundTruth);
        auto lowestCorrectRank = lca ? lca->rank() : taxon_rank::none;

        statistics.assign_known_correct(
            cls.best ? cls.best->rank() : taxon_rank::none,
            cls.groundTruth ? cls.groundTruth->rank() : taxon_rank::none,
            lowestCorrectRank);

        // check if taxa of assigned target are covered
        if(opt.taxonCoverage) {
            update_coverage_statistics(db, cls, statistics);
        }

    } else {
        statistics.assign(cls.best ? cls.best->rank() : taxon_rank::none);
    }
}



/*************************************************************************//**
 *
 * @brief estimate read counts per taxon at specific level
 *
 *****************************************************************************/
void estimate_abundance(const database& db, taxon_count_map& allTaxCounts, const taxon_rank rank) {
    if(rank != taxon_rank::Sequence) {
        //prune taxon below estimation rank
        taxon t{0,0,"",rank-1};
        auto begin = allTaxCounts.lower_bound(&t);
        for(auto taxCount = begin; taxCount != allTaxCounts.end();) {
            auto lineage = db.ranks(taxCount->first);
            const taxon* ancestor = nullptr;
            unsigned index = static_cast<unsigned>(rank);
            while(!ancestor && index < lineage.size())
                ancestor = lineage[index++];
            if(ancestor) {
                allTaxCounts[ancestor] += taxCount->second;
                taxCount = allTaxCounts.erase(taxCount);
            } else {
                ++taxCount;
            }
        }
    }

    std::unordered_map<const taxon*, std::vector<const taxon*> > taxChildren;
    std::unordered_map<const taxon*, query_id > taxWeights;
    taxWeights.reserve(allTaxCounts.size());

    //initialize weigths for fast lookup
    for(const auto& taxCount : allTaxCounts) {
        taxWeights[taxCount.first] = 0;
    }

    //for every taxon find its parent and add to their count
    //traverse allTaxCounts from leafs to root
    for(auto taxCount = allTaxCounts.rbegin(); taxCount != allTaxCounts.rend(); ++taxCount) {
        //find closest parent
        auto lineage = db.ranks(taxCount->first);
        const taxon* parent = nullptr;
        auto index = static_cast<std::uint8_t>(taxCount->first->rank()+1);
        while(index < lineage.size()) {
            parent = lineage[index++];
            if(parent && taxWeights.count(parent)) {
                //add own count to parent
                taxWeights[parent] += taxWeights[taxCount->first] + taxCount->second;
                //link from parent to child
                taxChildren[parent].emplace_back(taxCount->first);
                break;
            }
        }
    }

    //distribute counts to children and erase parents
    //traverse allTaxCounts from root to leafs
    for(auto taxCount = allTaxCounts.begin(); taxCount != allTaxCounts.end();) {
        auto children = taxChildren.find(taxCount->first);
        if(children != taxChildren.end()) {
            query_id sumChildren = taxWeights[taxCount->first];

            //distribute proportionally
            for(const auto& child : children->second) {
                allTaxCounts[child] += taxCount->second * (allTaxCounts[child]+taxWeights[child]) / sumChildren;
            }
            taxCount = allTaxCounts.erase(taxCount);
        }
        else {
            ++taxCount;
        }
    }
    //remaining tax counts are leafs
}



/*************************************************************************//**
 *
 * @brief compute alignment of top hits and optionally show it
 *
 *****************************************************************************/
void show_alignment(std::ostream& os,
                    const database& db,
                    const classification_output_options& opt,
                    const sequence_query& query,
                    const classification_candidates& tophits)
{
    //try to align to top target
    const taxon* tgtTax = tophits[0].tax;
    if(tgtTax && tgtTax->rank() == taxon_rank::Sequence) {
        const auto& src = tgtTax->source();
        try {
            //load candidate file and forward to sequence
            auto reader = make_sequence_reader(src.filename);
            reader->skip(src.index-1);

            if(reader->has_next()) {
                auto tgtSequ = reader->next().data;
                auto subject = make_view_from_window_range(
                               tgtSequ, tophits[0].pos,
                               db.target_sketcher().window_size(),
                               db.target_sketcher().window_stride());

                auto align = make_semi_global_alignment(query, subject);

                //print alignment to top candidate
                const auto w = db.target_sketcher().window_stride();
                const auto& comment = opt.format.tokens.comment;
                os  << '\n'
                    << comment << "  score  " << align.score
                    << "  aligned to "
                    << src.filename << " #" << src.index
                    << " in range [" << (w * tophits[0].pos.beg)
                    << ',' << (w * tophits[0].pos.end + w) << "]\n"
                    << comment << "  query  " << align.query << '\n'
                    << comment << "  target " << align.subject;
            }
        }
        catch(std::exception& e) {
            if(opt.showErrors) cerr << e.what() << '\n';
        }
    }
}



/*************************************************************************//**
 *
 * @brief print header line for mapping table
 *
 *****************************************************************************/
void show_query_mapping_header(std::ostream& os,
                               const classification_output_options& opt)
{
    if(opt.format.mapViewMode == map_view_mode::none) return;

    const auto& colsep = opt.format.tokens.column;

    os << opt.format.tokens.comment << "TABLE_LAYOUT: ";

    if(opt.format.showQueryIds) os << "query_id" << colsep;

    os << "query_header" << colsep;

    if(opt.evaluate.showGroundTruth) {
        show_taxon_header(os, opt.format, "truth_");
        os << colsep;
    }

    if(opt.analysis.showAllHits) os << "all_hits" << colsep;
    if(opt.analysis.showTopHits) os << "top_hits" << colsep;
    if(opt.analysis.showLocations) os << "candidate_locations" << colsep;

    show_taxon_header(os, opt.format);

    os << '\n';
}



/*************************************************************************//**
 *
 * @brief shows one query mapping line
 *        [query id], query_header, classification [, [top|all]hits list]
 *
 *****************************************************************************/
template<class Locations>
void show_query_mapping(
    std::ostream& os,
    const database& db,
    const classification_output_options& opt,
    const sequence_query& query,
    const classification& cls,
    const Locations& allhits)
{
    const auto& fmt = opt.format;

    if(fmt.mapViewMode == map_view_mode::none ||
        (fmt.mapViewMode == map_view_mode::mapped_only && !cls.best))
    {
        return;
    }

    const auto& colsep = fmt.tokens.column;

    if(fmt.showQueryIds) os << query.id << colsep;

    //print query header (first contiguous string only)
    auto l = query.header.find(' ');
    if(l != string::npos) {
        auto oit = std::ostream_iterator<char>{os, ""};
        std::copy(query.header.begin(), query.header.begin() + l, oit);
    }
    else {
        os << query.header;
    }
    os << colsep;

    if(opt.evaluate.showGroundTruth) {
        show_taxon(os, db, opt.format, cls.groundTruth);
        os << colsep;
    }
    if(opt.analysis.showAllHits) {
        show_matches(os, db, allhits, fmt.lowestRank);
        os << colsep;
    }
    if(opt.analysis.showTopHits) {
        show_candidates(os, db, cls.candidates, fmt.lowestRank);
        os << colsep;
    }
    if(opt.analysis.showLocations) {
        show_candidate_ranges(os, db, cls.candidates);
        os << colsep;
    }

    show_taxon(os, db, fmt, cls.best);

    if(opt.analysis.showAlignment && cls.best) {
        show_alignment(os, db, opt, query, cls.candidates);
    }

    os << '\n';
}



/*************************************************************************//**
 *
 * @brief filter out targets which have a coverage percentage below a percentile
 *        of all coverage percentages
 *
 *****************************************************************************/
void filter_targets_by_coverage(
    const database& db,
    matches_per_target& tgtMatches,
    const float percentile)
{
    using coverage_percentage = std::pair<target_id, float>;

    vector<coverage_percentage> coveragePercentages;
    coveragePercentages.reserve(tgtMatches.size());

    float coveragePercentagesSum = 0;

    //calculate coverage percentages
    for(const auto& mapping : tgtMatches) {
        target_id target = mapping.first;
        const taxon* tax = db.taxon_of_target(target);
        const window_id targetSize = tax->source().windows;
        std::unordered_set<window_id> hitWindows;
        for(const auto& candidate : mapping.second) {
            for(const auto& windowMatch : candidate.matches) {
                hitWindows.emplace(windowMatch.win);
            }
        }
        const float covP = float(hitWindows.size()) / targetSize;
        coveragePercentagesSum += covP;
        coveragePercentages.emplace_back(target, covP);
    }

    //sort by coverage descending
    std::sort(coveragePercentages.begin(), coveragePercentages.end(),
        [](coverage_percentage& a, coverage_percentage& b){
            return a.second < b.second;
    });

    //filter out targets
    float coveragePercentagesPartSum = 0;
    for(auto it = coveragePercentages.begin(); it != coveragePercentages.end(); ++it) {
        coveragePercentagesPartSum += it->second;
        if(coveragePercentagesPartSum > percentile * coveragePercentagesSum) {
            // coveragePercentages.resize(std::distance(it, coveragePercentages.rend()));
            break;
        }
        tgtMatches.erase(it->first);
    }
}



/*************************************************************************//**
 *
 * @brief per-batch buffer for output and (target -> hits) lists
 *
 *****************************************************************************/
struct query_mapping
{
    sequence_query query;
    classification cls;
    // matches_per_location allhits;
};

using query_mappings = std::vector<query_mapping>;


/*************************************************************************//**
 *
 * @brief redo the classification of all reads using only targets in tgtMatches
 *
 *****************************************************************************/
void redo_classification_batched(
    moodycamel::ConcurrentQueue<query_mappings>& queryMappingsQueue,
    const matches_per_target& tgtMatches,
    const database& db, const query_options& opt,
    classification_results& results,
    taxon_count_map& allTaxCounts)
{
    //parallel
    std::vector<std::future<void>> threads;
    std::mutex mtx;

    for(int threadId = 0; threadId < opt.performance.numThreads; ++threadId) {
        threads.emplace_back(std::async(std::launch::async, [&, threadId] {

            query_mappings mappings;

            while(queryMappingsQueue.size_approx()) {
                if(queryMappingsQueue.try_dequeue(mappings)) {
                    std::ostringstream bufout;
                    taxon_count_map taxCounts;

                    for(auto& mapping : mappings) {
                        // classify using only targets left in tgtMatches
                        update_classification(db, opt.classify, mapping.cls, tgtMatches);

                        evaluate_classification(db, opt.output.evaluate, mapping.query, mapping.cls, results.statistics);

                        show_query_mapping(bufout, db, opt.output, mapping.query, mapping.cls, match_locations{});

                        if(opt.make_tax_counts() && mapping.cls.best) {
                            ++taxCounts[mapping.cls.best];
                        }
                    }
                    std::lock_guard<std::mutex> lock(mtx);
                    if(opt.make_tax_counts()) {
                        //add batch (taxon->read count) to global counts
                        for(const auto& taxCount : taxCounts)
                            allTaxCounts[taxCount.first] += taxCount.second;
                    }
                    results.perReadOut << bufout.str();
                }
            }
        }));
    }

    //wait for all threads to finish
    for(auto& thread : threads) {
        thread.get();
    }
}



/*************************************************************************//**
 *
 * @brief per-batch buffer for output and (target -> hits) lists
 *
 *****************************************************************************/
struct mappings_buffer
{
    std::ostringstream out;
    query_mappings queryMappings;
    matches_per_target hitsPerTarget;
    taxon_count_map taxCounts;
};

/*************************************************************************//**
 *
 * @brief default classification scheme with
 *        additional target->hits list generation and output
 *        try to map each read to a taxon with the lowest possible rank
 *
 *****************************************************************************/
void map_queries_to_targets_default(
    const vector<string>& infiles,
    const database& db, const query_options& opt,
    classification_results& results)
{
    const auto& fmt = opt.output.format;

    //global target -> query_id/win:hits... list
    matches_per_target tgtMatches;

    moodycamel::ConcurrentQueue<query_mappings> queryMappingsQueue;

    //global taxon -> read count
    taxon_count_map allTaxCounts;

    if(opt.output.evaluate.precision || opt.output.evaluate.determineGroundTruth) {
        //groundtruth may be outside of target lineages
        //cache lineages of *all* taxa
        db.update_cached_lineages(taxon_rank::none);
    }

    //input queries are divided into batches;
    //each batch might be processed by a different thread;
    //the following 4 lambdas define actions that should be performed
    //on such a batch and its associated buffer;
    //the batch buffer can be used to cache intermediate results

    //creates an empty batch buffer
    const auto makeBatchBuffer = [] { return mappings_buffer(); };

    //updates buffer with the database answer of a single query
    const auto processQuery = [&](mappings_buffer& buf,
        const sequence_query& query, const auto& allhits)
    {
        if(query.empty()) return;

        auto cls = classify(db, opt.classify, query, allhits);

        if(opt.output.analysis.showHitsPerTargetList || opt.classify.covPercentile > 0) {
            //insert all candidates with at least 'hitsMin' hits into
            //target -> match list
            buf.hitsPerTarget.insert(query.id, allhits, cls.candidates,
                                     opt.classify.hitsMin);
        }

        if(opt.classify.covPercentile > 0) {
            sequence_query qinfo;
            qinfo.id = query.id;
            qinfo.header = query.header;
            //save query mapping for post processing
            buf.queryMappings.emplace_back(query_mapping{std::move(qinfo), std::move(cls)});
        }
        else {
            //use classification as is
            if(opt.make_tax_counts() && cls.best) {
                ++buf.taxCounts[cls.best];
            }

            evaluate_classification(db, opt.output.evaluate, query, cls, results.statistics);

            show_query_mapping(buf.out, db, opt.output, query, cls, allhits);
        }
    };

    //runs before a batch buffer is discarded
    const auto finalizeBatch = [&] (mappings_buffer&& buf) {
        if(opt.output.analysis.showHitsPerTargetList || opt.classify.covPercentile > 0) {
            //merge batch (target->hits) lists into global one
            tgtMatches.merge(std::move(buf.hitsPerTarget));
        }
        if(opt.classify.covPercentile > 0) {
            //move mappings to global map
            queryMappingsQueue.enqueue(buf.queryMappings);
            buf.queryMappings.clear();
        }
        else {
            if(opt.make_tax_counts()) {
                //add batch (taxon->read count) to global counts
                for(const auto& taxCount : buf.taxCounts)
                    allTaxCounts[taxCount.first] += taxCount.second;
            }
            //write output buffer to output stream when batch is finished
            results.perReadOut << buf.out.str();
        }
    };

    //runs if something needs to be appended to the output
    const auto appendToOutput = [&] (const std::string& msg) {
        results.perReadOut << fmt.tokens.comment << msg << '\n';
    };

    //run (parallel) database queries according to processing options
    query_database(infiles, db, opt.pairing, opt.performance,
                   makeBatchBuffer, processQuery, finalizeBatch,
                   appendToOutput);

    //filter all matches by coverage
    if(opt.classify.covPercentile > 0) {
        filter_targets_by_coverage(db, tgtMatches, opt.classify.covPercentile);

        redo_classification_batched(queryMappingsQueue, tgtMatches, db, opt, results, allTaxCounts);
    }

    const auto& analysis = opt.output.analysis;

    if(analysis.showHitsPerTargetList) {
        tgtMatches.sort_match_lists();
        show_matches_per_targets(results.perTargetOut, db, tgtMatches, fmt);
    }

    if(analysis.showTaxAbundances) {
        show_abundances(results.perTaxonOut, allTaxCounts,
                        results.statistics, fmt);
    }

    if(analysis.showAbundanceEstimatesOnRank != taxonomy::rank::none) {
        estimate_abundance(db, allTaxCounts, analysis.showAbundanceEstimatesOnRank);

        show_abundance_estimates(results.perTaxonOut,
                                 analysis.showAbundanceEstimatesOnRank,
                                 allTaxCounts, results.statistics, fmt);
    }
}



/*************************************************************************//**
 *
 * @brief default classification scheme & output
 *        try to map each read to a taxon with the lowest possible rank
 *
 *****************************************************************************/
void map_queries_to_targets(const vector<string>& infiles,
                            const database& db, const query_options& opt,
                            classification_results& results)
{
    if(opt.output.format.mapViewMode != map_view_mode::none) {
        show_query_mapping_header(results.perReadOut, opt.output);
    }

    map_queries_to_targets_default(infiles, db, opt, results);
}



/*************************************************************************//**
 *
 * @brief needed for 'merge' mode: default classification scheme & output
 *        try to map candidates to a taxon with the lowest possible rank
 *
 *****************************************************************************/
void map_candidates_to_targets(const vector<string>& queryHeaders,
                               const vector<classification_candidates>& queryCandidates,
                               const database& db, const query_options& opt,
                               classification_results& results)
{
    if(opt.output.format.mapViewMode != map_view_mode::none) {
        show_query_mapping_header(results.perReadOut, opt.output);
    }

    //taxon -> read count
    taxon_count_map allTaxCounts;

    for(size_t i = 0; i < queryHeaders.size(); ++i) {
        sequence_query query{i+1, std::move(queryHeaders[i]), {}};

        classification cls { queryCandidates[i] };
        cls.best = classify(db, opt.classify, cls.candidates);

        if(opt.make_tax_counts() && cls.best) {
            ++allTaxCounts[cls.best];
        }

        evaluate_classification(db, opt.output.evaluate, query, cls, results.statistics);

        show_query_mapping(results.perReadOut, db, opt.output, query, cls, match_locations{});
    }

    const auto& analysis = opt.output.analysis;
    if(analysis.showTaxAbundances) {
        show_abundances(results.perTaxonOut, allTaxCounts,
                        results.statistics, opt.output.format);
    }

    if(analysis.showAbundanceEstimatesOnRank != taxonomy::rank::none) {
        estimate_abundance(db, allTaxCounts, analysis.showAbundanceEstimatesOnRank);

        show_abundance_estimates(results.perTaxonOut,
                                 analysis.showAbundanceEstimatesOnRank,
                                 allTaxCounts,
                                 results.statistics, opt.output.format);
    }
}


} // namespace mc
