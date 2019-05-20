/* Copyright (c) 2018, NVIDIA CORPORATION. All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <vector>
#include <map>
#include <numeric>
#include <functional>
#include <set>

#include "util/numeric.hpp"
#include "workload/problem-config.hpp"

namespace mapspace
{

//--------------------------------------------//
//           IndexFactorizationSpace          //
//--------------------------------------------//

class IndexFactorizationSpace
{
 private:
  problem::PerProblemDimension<Factors> dimension_factors_;
  CartesianCounterDynamic tiling_counter_;

 public:
  IndexFactorizationSpace() :
      tiling_counter_(problem::NumDimensions)
  { }

  void Init(const problem::WorkloadConfig &problem_config,
            std::map<problem::Dimension, std::uint64_t> cofactors_order,
            std::map<problem::Dimension, std::map<unsigned, unsigned long>> prefactors =
            std::map<problem::Dimension, std::map<unsigned, unsigned long>>()
           )
  {
    problem::PerProblemDimension<uint128_t> counter_base;
    for (int idim = 0; idim < int(problem::NumDimensions); idim++)
    {
      auto dim = problem::Dimension(idim);
      if (prefactors.find(dim) == prefactors.end())
        dimension_factors_[idim] = Factors(problem_config.getBound(dim), cofactors_order[dim]);
      else
        dimension_factors_[idim] = Factors(problem_config.getBound(dim), cofactors_order[dim], prefactors[dim]);
      counter_base[idim] = dimension_factors_[idim].size();
    }

    tiling_counter_.Init(counter_base);

    std::cout << "Initializing Index Factorization subspace." << std::endl;
    for (int dim = 0; dim < int(problem::NumDimensions); dim++)
    {
      std::cout << "  Factorization options along problem dimension " << problem::Dimension(dim) << " = " << counter_base[dim] << std::endl;
    }
  }

  unsigned long GetFactor(uint128_t nest_id, problem::Dimension dim, unsigned level)
  {
    auto idim = unsigned(dim);
    tiling_counter_.Set(nest_id);
    auto cartesian_idx = tiling_counter_.Read();    
    return dimension_factors_[idim][std::uint64_t(cartesian_idx[idim])][level];
  }

  uint128_t Size() const
  {
    return tiling_counter_.EndInteger();
  }
};

//--------------------------------------------//
//              PermutationSpace              //
//--------------------------------------------//

class PermutationSpace
{
 private:
  std::uint64_t num_levels_;
  struct Pattern
  {
    std::vector<problem::Dimension> baked_prefix;
    std::vector<problem::Dimension> permutable_suffix;
  };
  std::map<unsigned, Pattern> patterns_;
  std::vector<problem::Dimension> canonical_pattern_;
  std::map<unsigned, std::uint64_t> size_;    
  Factoradic<problem::Dimension> factoradic_;

 public:
  PermutationSpace()
  {
    for (unsigned i = 0; i < unsigned(problem::NumDimensions); i++)
    {
      canonical_pattern_.push_back(problem::Dimension(i));
    }
  }

  void Init(uint64_t num_levels)
  {
    num_levels_ = num_levels;
    patterns_.clear();
    size_.clear();
  }

  void InitLevelCanonical(uint64_t level)
  {
    InitLevel(level, canonical_pattern_);
  }

  void InitLevel(uint64_t level, std::vector<problem::Dimension> user_prefix,
                 std::vector<problem::Dimension> pruned_dimensions = {})
  {
    assert(level < num_levels_);

    // Merge pruned dimensions with user prefix, with all unit factors at the
    // beginning followed by user-specified non-unit factors, i.e.,
    // <unit-factors><user-specified-non-unit-factors><free-non-unit-factors>
    // <unit-factors><user-specified-non-unit-factors> = baked_prefix
    // <free-non-unit-factors> = permutable_suffix
    std::vector<problem::Dimension> baked_prefix = pruned_dimensions;

    for (auto dim : user_prefix)
      if (std::find(pruned_dimensions.begin(), pruned_dimensions.end(), dim) == pruned_dimensions.end())
        baked_prefix.push_back(dim);

    std::set<problem::Dimension> unspecified_dimensions;
    for (unsigned i = 0; i < unsigned(problem::NumDimensions); i++)
      unspecified_dimensions.insert(problem::Dimension(i));
    
    for (auto& dim : baked_prefix)
      unspecified_dimensions.erase(dim);
    
    std::vector<problem::Dimension> permutable_suffix;
    for (auto& dim : unspecified_dimensions)
      permutable_suffix.push_back(dim);

    assert(baked_prefix.size() + permutable_suffix.size() == unsigned(problem::NumDimensions));

    patterns_[level] = { baked_prefix, permutable_suffix };
    size_[level] = factoradic_.Factorial(permutable_suffix.size());
  }

  std::vector<std::vector<problem::Dimension>> GetPatterns(uint128_t id)
  {
    std::vector<std::vector<problem::Dimension>> retval;

    for (unsigned level = 0; level < num_levels_; level++)
    {
      auto& pattern = patterns_.at(level);
      if (pattern.baked_prefix.size() == unsigned(problem::NumDimensions))
      {
        retval.push_back(pattern.baked_prefix);
      }
      else
      {
        std::vector<problem::Dimension> permuted_suffix = pattern.permutable_suffix;
        factoradic_.Permute(permuted_suffix.data(), permuted_suffix.size(),
                            std::uint64_t(id % size_.at(level)));
        id = id / size_.at(level);
        std::vector<problem::Dimension> final_pattern = pattern.baked_prefix;
        final_pattern.insert(final_pattern.end(), permuted_suffix.begin(), permuted_suffix.end());

        retval.push_back(final_pattern);
      }
    }

    return retval;
  }

  uint128_t Size() const
  {
    uint128_t product = 1;
    for (unsigned level = 0; level < num_levels_; level++)
    {
      product *= size_.at(level);
    }
    return product;
  }
};

//--------------------------------------------//
//              SpatialSplitSpace             //
//--------------------------------------------//

class SpatialSplitSpace
{
 private:
  // Ugh. The number of levels given to us is the total number
  // of tiling levels. Of these, only a subset are spatial. We
  // need to remember (a) which of these are spatial, and (b)
  // which of the spatial ones have user-specified splits.
  std::uint64_t num_levels_;
  std::map<unsigned, bool> is_user_specified_;
  std::map<unsigned, std::uint32_t> user_splits_;
  std::map<unsigned, std::size_t> size_;
  std::map<unsigned, unsigned> unit_factors_;

  uint64_t n_;
  bool is_fixed_;
  uint64_t fixed_;
  
 public:
  SpatialSplitSpace() {}

  void Init(uint64_t num_levels)
  {
    num_levels_ = num_levels;
    is_user_specified_.clear();
    user_splits_.clear();
    size_.clear();
    unit_factors_.clear();
  }

  void InitLevel(uint64_t level, unsigned unit_factors = 0)
  {
    assert(level < num_levels_);
    is_user_specified_[level] = false;
    unit_factors_[level] = unit_factors;
    size_[level] = int(problem::NumDimensions) + 1 - unit_factors;
  }

  void InitLevelUserSpecified(uint64_t level, std::uint32_t user_split)
  {
    assert(level < num_levels_);
    is_user_specified_[level] = true;
    user_splits_[level] = user_split;
    size_[level] = 1;
  }

  std::map<unsigned, std::uint32_t> GetSplits(uint128_t id)
  {
    std::map<unsigned, std::uint32_t> retval;
    
    for (unsigned level = 0; level < num_levels_; level++)
    {
      // Is this a spatial level (i.e., was this level initialized at all)?
      auto it_is_user_specified = is_user_specified_.find(level);
      if (it_is_user_specified != is_user_specified_.end())
      {
        // Was this level user-specified?
        if (it_is_user_specified->second)
        {
          // User-specified
          retval[level] = user_splits_.at(level);
        }
        else
        {
          // Variable
          retval[level] = unit_factors_.at(level) + std::uint32_t(id % size_.at(level));
          id = id / size_.at(level);
        }
      }      
    }

    return retval;
  }

  uint128_t Size() const
  {
    uint128_t retval = 1;
    for (auto& it : size_)
      retval *= uint128_t(it.second);
    return retval;
  }  
};

} // namespace mapspace
